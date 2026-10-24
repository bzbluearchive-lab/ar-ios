// RemoteLoad.h —— 运行时把 dylib 加载进另一个进程（形态 β 的核心）
//
// iOS 注意：<mach/mach_vm.h> 在 iOS SDK 里被 #error 禁用，所以一律用 vm_* 系列
// （vm_allocate / vm_write / thread_*），它们在 <mach/vm_map.h> 声明，对远程 task 同样有效。
//
// 支点：libdyld 在 dyld shared cache 里，所有进程映射到同一地址，
// 所以我们进程里 dlsym(RTLD_DEFAULT, "dlopen") 的地址在目标进程里照样有效。
//
// 进程查找有两条路（沙盒 app 用 sysctl 可能看不到别的进程名，所以必须有兜底）：
//   1. sysctl KERN_PROC_ALL -> kp_proc.p_comm 按名字匹配
//   2. libproc 的 proc_listallpids + proc_pidpath 按可执行文件路径匹配
// 两条都失败时，dump_procs() 会把能看到的东西打出来，便于诊断。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>
#include <sys/sysctl.h>

// iOS SDK 不提供 <libproc.h>，但这两个符号由 libSystem 导出，
// 所以手写原型即可（jailbreak 工具普遍这么做）。
extern "C" {
int proc_listallpids(void* buffer, int buffersize);
int proc_pidpath(int pid, void* buffer, uint32_t buffersize);
}
static const int kProcPidPathMax = 1024;   // 对应 macOS 的 PROC_PIDPATHINFO_MAXSIZE

namespace rload {

struct Ctx {
    mach_port_t  task    = MACH_PORT_NULL;
    pid_t        pid     = 0;
    vm_address_t strAddr = 0;
    vm_address_t stack   = 0;
};

// ---------------- 进程查找：路线 1，按 comm 名字 ----------------
inline int list_procs_sysctl(struct kinfo_proc** out) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t sz = 0;
    int rc = sysctl(mib, 4, nullptr, &sz, nullptr, 0);
    if (rc != 0 || sz == 0) { *out = nullptr; return rc ? rc : -1; }
    struct kinfo_proc* p = (struct kinfo_proc*)malloc(sz);
    if (!p) { *out = nullptr; return -2; }
    rc = sysctl(mib, 4, p, &sz, nullptr, 0);
    if (rc != 0) { free(p); *out = nullptr; return rc; }
    *out = p;
    return (int)(sz / sizeof(struct kinfo_proc));
}

inline pid_t find_pid_comm(const char* name) {
    struct kinfo_proc* p = nullptr;
    int n = list_procs_sysctl(&p);
    if (n <= 0 || !p) return 0;
    pid_t found = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(p[i].kp_proc.p_comm, name)) { found = p[i].kp_proc.p_pid; break; }
    }
    free(p);
    return found;
}

// ---------------- 进程查找：路线 2，按可执行文件路径 ----------------
inline pid_t find_pid_path(const char* needle) {
    int cap = proc_listallpids(nullptr, 0);
    if (cap <= 0) return 0;
    cap += 32;
    pid_t* pids = (pid_t*)calloc((size_t)cap, sizeof(pid_t));
    if (!pids) return 0;
    int cnt = proc_listallpids(pids, cap * (int)sizeof(pid_t));
    pid_t found = 0;
    for (int i = 0; i < cnt; i++) {
        char path[kProcPidPathMax];
        memset(path, 0, sizeof(path));
        if (proc_pidpath(pids[i], path, sizeof(path)) > 0 && strstr(path, needle)) {
            found = pids[i];
            break;
        }
    }
    free(pids);
    return found;
}

inline pid_t find_pid(const char* name) {
    pid_t p = find_pid_comm(name);
    if (p) { printf("[rload] 按名字命中 %s pid=%d\n", name, p); return p; }
    p = find_pid_path(name);
    if (p) { printf("[rload] 按路径命中 %s pid=%d\n", name, p); return p; }
    printf("[rload] 两条路都没找到 %s\n", name);
    return 0;
}

// ---------------- 诊断：把枚举得到的东西打出来 ----------------
inline void dump_procs(int maxPrint) {
    struct kinfo_proc* p = nullptr;
    int n = list_procs_sysctl(&p);
    printf("[diag] sysctl count=%d\n", n);
    if (n > 0 && p) {
        for (int i = 0; i < n && i < maxPrint; i++) {
            printf("  sysctl pid=%d comm='%s'\n", p[i].kp_proc.p_pid, p[i].kp_proc.p_comm);
        }
        free(p);
    }
    int cap = proc_listallpids(nullptr, 0);
    printf("[diag] proc_listallpids cap=%d\n", cap);
    if (cap > 0) {
        cap += 32;
        pid_t* pids = (pid_t*)calloc((size_t)cap, sizeof(pid_t));
        if (pids) {
            int cnt = proc_listallpids(pids, cap * (int)sizeof(pid_t));
            printf("[diag] proc_listallpids count=%d\n", cnt);
            for (int i = 0; i < cnt && i < maxPrint; i++) {
                char path[kProcPidPathMax];
                memset(path, 0, sizeof(path));
                int r = proc_pidpath(pids[i], path, sizeof(path));
                printf("  proc pid=%d path_rc=%d path='%s'\n", pids[i], r, path);
            }
            free(pids);
        }
    }
}

// ---------------- 在目标进程里分配并写入 ----------------
inline bool write_remote(Ctx& c, const void* src, size_t len, vm_address_t& out) {
    vm_address_t addr = 0;
    kern_return_t kr = vm_allocate(c.task, &addr, (vm_size_t)len, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) { printf("[rload] vm_allocate -> %d\n", kr); return false; }
    kr = vm_write(c.task, addr, (vm_offset_t)src, (mach_msg_type_number_t)len);
    if (kr != KERN_SUCCESS) { printf("[rload] vm_write -> %d\n", kr); return false; }
    out = addr;
    return true;
}

// ---------------- 远程调用 ----------------
inline kern_return_t call_remote(Ctx& c, uintptr_t fnAddr,
                                 uint64_t a0, uint64_t a1, uintptr_t retTo) {
    mach_port_t th = MACH_PORT_NULL;
    kern_return_t kr = thread_create(c.task, &th);
    if (kr != KERN_SUCCESS) { printf("[rload] thread_create -> %d\n", kr); return kr; }

    arm_thread_state64_t st;
    memset(&st, 0, sizeof(st));
    st.__x[0] = a0;
    st.__x[1] = a1;
    st.__sp   = (uint64_t)((c.stack + 0x10000) & ~0xFull);   // 16 字节对齐
    st.__pc   = (uint64_t)fnAddr;
    st.__lr   = (uint64_t)(retTo ? retTo : 0);

    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st,
                          ARM_THREAD_STATE64_COUNT);
    if (kr != KERN_SUCCESS) {
        printf("[rload] thread_set_state -> %d\n", kr);
        mach_port_deallocate(mach_task_self(), th);
        return kr;
    }
    kr = thread_resume(th);
    if (kr != KERN_SUCCESS) printf("[rload] thread_resume -> %d\n", kr);
    return kr;
}

// ---------------- 主流程 ----------------
inline bool inject_pid(pid_t pid, const char* dylibPath) {
    Ctx c;
    c.pid = pid;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &c.task);
    printf("[rload] task_for_pid(%d) -> %d\n", pid, kr);
    if (kr != KERN_SUCCESS) return false;

    size_t plen = strlen(dylibPath) + 1;
    if (!write_remote(c, dylibPath, plen, c.strAddr)) return false;

    if (vm_allocate(c.task, &c.stack, 0x10000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        printf("[rload] vm_allocate(stack) failed\n");
        return false;
    }

    uintptr_t dlopenAddr = (uintptr_t)dlsym(RTLD_DEFAULT, "dlopen");
    uintptr_t pthExit    = (uintptr_t)dlsym(RTLD_DEFAULT, "pthread_exit");
    printf("[rload] dlopen=0x%lx pthread_exit=0x%lx\n",
           (unsigned long)dlopenAddr, (unsigned long)pthExit);
    if (!dlopenAddr) return false;

    kr = call_remote(c, dlopenAddr, (uint64_t)c.strAddr, (uint64_t)RTLD_NOW, pthExit);
    return kr == KERN_SUCCESS;
}

inline bool inject(const char* procName, const char* dylibPath) {
    pid_t pid = find_pid(procName);
    if (!pid) return false;
    return inject_pid(pid, dylibPath);
}

} // namespace rload
