// TaskPorts.h —— 用 processor_set_tasks 取任意进程的 task port
//
// 为什么单独写这一层：
//   task_for_pid 对"平台二进制/其他团队的 App"有额外限制，我们在 SpringBoard 上
//   已经吃过 0x10000003 的亏。而 `processor_set_tasks` 是另一条路：
//   它返回"某个 processor set 里所有 task 的 send right"，
//   带 task_for_pid-allow / com.apple.system-task-ports 的进程通常能拿到。
//   这也是商用产品（播客.RS.R）导入表里出现它的原因。
//
// 拿到 task port 之后，vm_read_overwrite / vm_write 就能直接对目标进程读写，
// 不再受 sandbox 与 task_for_pid 的限制。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/task_info.h>

// iOS SDK 不暴露这两个，自己声明（libSystem 里有）
extern "C" {
kern_return_t processor_set_default(host_t host, mach_port_t* pset);
kern_return_t processor_set_tasks(mach_port_t pset,
                                 mach_port_array_t* task_list,
                                 mach_msg_type_number_t* task_count);
kern_return_t pid_for_task(mach_port_t task, int* pid);
}

namespace taskports {

struct Found {
    mach_port_t port = MACH_PORT_NULL;
    int         pid = 0;
};

// 枚举 processor set 里的所有 task，按 pid 找目标
static inline Found find_by_pid(int wantPid, int* totalOut = nullptr) {
    Found out;
    if (totalOut) *totalOut = 0;

    mach_port_t pset = MACH_PORT_NULL;
    kern_return_t kr = processor_set_default(mach_host_self(), &pset);
    printf("[tports] processor_set_default -> %d pset=%u\n", kr, pset);
    if (kr != KERN_SUCCESS) return out;

    mach_port_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    kr = processor_set_tasks(pset, &list, &count);
    printf("[tports] processor_set_tasks -> %d count=%u\n", kr, count);
    if (kr != KERN_SUCCESS || !list) {
        mach_port_deallocate(mach_task_self(), pset);
        return out;
    }
    if (totalOut) *totalOut = (int)count;

    for (mach_msg_type_number_t i = 0; i < count; i++) {
        int pid = 0;
        if (pid_for_task(list[i], &pid) == KERN_SUCCESS && pid == wantPid && !out.port) {
            // 保留这个端口（把自己的一份引用留住）
            mach_port_mod_refs(mach_task_self(), list[i], MACH_PORT_RIGHT_SEND, 1);
            out.port = list[i];
            out.pid = pid;
            printf("[tports] 命中 pid=%d -> task port=%u\n", pid, out.port);
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }

    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(mach_port_t));
    mach_port_deallocate(mach_task_self(), pset);

    if (!out.port) printf("[tports] 没找到 pid=%d\n", wantPid);
    return out;
}

// 用拿到的 task port 做一次读验证：读目标主镜像头
static inline bool verify(mach_port_t task, int pid) {
    if (!task) return false;

    task_basic_info_data_t bi;
    mach_msg_type_number_t bc = TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(task, TASK_BASIC_INFO, (task_info_t)&bi, &bc);
    printf("[tports] task_info -> %d suspend=%d resident=%llu MB\n", kr,
           kr == KERN_SUCCESS ? bi.suspend_count : -1,
           (unsigned long long)(kr == KERN_SUCCESS ? bi.resident_size / 1048576 : 0));

    task_dyld_info_data_t di;
    mach_msg_type_number_t dc = TASK_DYLD_INFO_COUNT;
    kr = task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &dc);
    if (kr != KERN_SUCCESS) {
        printf("[tports] TASK_DYLD_INFO 失败 %d（没有 task port 权限）\n", kr);
        return false;
    }
    uint64_t allInfo = di.all_image_info_addr;
    uint32_t nimg = 0;
    uint64_t arr = 0;
    vm_size_t got = 0;
    vm_read_overwrite(task, (vm_address_t)(allInfo + 4), 4, (vm_address_t)&nimg, &got);
    vm_read_overwrite(task, (vm_address_t)(allInfo + 8), 8, (vm_address_t)&arr, &got);
    uint64_t headerAddr = 0;
    vm_read_overwrite(task, (vm_address_t)arr, 8, (vm_address_t)&headerAddr, &got);

    uint32_t magic = 0;
    kr = vm_read_overwrite(task, (vm_address_t)headerAddr, 4, (vm_address_t)&magic, &got);
    printf("[tports] pid=%d 镜像数=%u 主镜像=0x%llx magic=0x%08x read=%d\n",
           pid, nimg, (unsigned long long)headerAddr, magic, kr);
    return (magic == 0xFEEDFACF || magic == 0xFEEDFACE);
}

} // namespace taskports
