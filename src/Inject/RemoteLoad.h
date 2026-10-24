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

// ---------------- 跨进程读（用 vm_read_overwrite，对远程 task 有效） ----------------
inline bool read_remote_mem(mach_port_t task, uint64_t addr, void* out, size_t n) {
    if (!addr || !out || !n) return false;
    vm_size_t got = 0;
    kern_return_t kr = vm_read_overwrite(task, (vm_address_t)addr, (vm_size_t)n,
                                         (vm_address_t)out, &got);
    return kr == KERN_SUCCESS && got == (vm_size_t)n;
}

// 读目标进程主可执行文件的路径。
// 路径来自 dyld_all_image_infos -> infoArray[0].imageFilePath。
// 这是"不依赖进程枚举"识别进程的唯一可靠办法。
inline bool read_remote_path(mach_port_t task, char* out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = 0;

    task_dyld_info_data_t di;
    mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
    if (task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &cnt) != KERN_SUCCESS) return false;

    uint64_t base = di.all_image_info_addr;
    if (!base) return false;

    // struct dyld_all_image_infos: +0 version(u32) +4 infoArrayCount(u32) +8 infoArray(ptr)
    uint32_t imgCount = 0;
    if (!read_remote_mem(task, base + 4, &imgCount, 4)) return false;
    uint64_t arr = 0;
    if (!read_remote_mem(task, base + 8, &arr, 8)) return false;
    if (!imgCount || !arr) return false;

    // struct dyld_image_info: +0 imageLoadAddress +8 imageFilePath +16 imageFileModDate
    uint64_t pathPtr = 0;
    if (!read_remote_mem(task, arr + 8, &pathPtr, 8)) return false;
    if (!pathPtr) return false;

    memset(out, 0, cap);
    return read_remote_mem(task, pathPtr, out, cap - 1);
}

// ---------------- 暴力定位：不依赖任何进程枚举 API ----------------
// 沙盒 app 的 sysctl / libproc 都被拒（实测 count=-1 / cap=0），
// 所以逐个 pid 试 task_for_pid，成功者再读它的可执行文件路径来认身份。
// 返回命中的 pid；未命中返回 0。verbose 时把每个成功的 pid 打出来。
inline pid_t find_by_bruteforce(const char* nameNeedle, int maxPid, bool verbose) {
    printf("[brute] 扫描 pid 1..%d 找 %s\n", maxPid, nameNeedle);
    int hits = 0;
    pid_t found = 0;
    for (pid_t p = 1; p <= maxPid; p++) {
        mach_port_t t = MACH_PORT_NULL;
        kern_return_t kr = task_for_pid(mach_task_self(), p, &t);
        if (kr != KERN_SUCCESS) continue;
        hits++;
        char path[1024];
        bool ok = read_remote_path(t, path, sizeof(path));
        if (verbose || (ok && strstr(path, nameNeedle))) {
            printf("[brute] pid=%-5d path=%s\n", p, ok ? path : "(unknown)");
        }
        if (ok && strstr(path, nameNeedle) && !found) found = p;
        mach_port_deallocate(mach_task_self(), t);
        if (found) break;
    }
    printf("[brute] task_for_pid 成功 %d 个, 命中=%d\n", hits, found);
    return found;
}

// 按多个关键词扫描，打印所有命中者，返回第一个命中的 pid。
// 用途：找游戏进程（普通 app 的路径可读），或找 SpringBoard（平台二进制，通常读不到路径）。
inline pid_t find_by_needles(int maxPid, const char* const* needles, int n) {
    printf("[find] 扫描 pid 1..%d，关键词 %d 个\n", maxPid, n);
    pid_t found = 0;
    int readable = 0;
    for (pid_t p = 1; p <= maxPid; p++) {
        mach_port_t t = MACH_PORT_NULL;
        if (task_for_pid(mach_task_self(), p, &t) != KERN_SUCCESS) continue;
        char path[1024];
        bool ok = read_remote_path(t, path, sizeof(path));
        mach_port_deallocate(mach_task_self(), t);
        if (!ok) continue;
        readable++;
        for (int i = 0; i < n; i++) {
            if (path[0] && strstr(path, needles[i])) {
                printf("[find] 命中 pid=%-5d %s\n", p, path);
                if (!found) found = p;
                break;
            }
        }
    }
    printf("[find] 路径可读 %d 个进程，首个命中 pid=%d\n", readable, found);
    return found;
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
    if (!pid) {
        printf("[rload] 枚举找不到，改用暴力扫描\n");
        pid = find_by_bruteforce(procName, 1024, true);
    }
    if (!pid) { printf("[rload] 暴力扫描也没命中\n"); return false; }
    return inject_pid(pid, dylibPath);
}

} // namespace rload
