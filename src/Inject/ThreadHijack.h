// ThreadHijack.h —— 劫持目标已有线程执行我们的代码（thread_create 失败时的备选）
//
// 为什么需要：
//   我们用 thread_create + thread_set_state 新建线程，把 PC 指到注入代码，
//   在目标进程里看不到任何执行痕迹。怀疑原因是新线程缺少必要的线程环境
//   （arm64 的 x18 是 TLS 指针、异常端口、栈守卫、线程结构里的其它字段）。
//   劫持一个已有线程可以继承它完整的线程环境。
//
// 代价与风险（必须知道）：
//   - 被劫持的线程会短暂偏离原流程，如果它此刻正持锁/在临界区，可能造成死锁或
//     状态不一致 —— 所以只作为 thread_create 失败后的备选，且只执行"短、幂等"的代码。
//   - 执行完必须把寄存器原样还原，否则目标大概率崩溃。
//   - 我们的入口函数不允许返回（返回会跳回 LR），所以注入的代码必须自己 park 住；
//     这里的等待逻辑是"轮询 PC 是否已离开入口"，超时后强制还原。

#pragma once
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>
#include <mach/vm_map.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace hijack {
// 扫描目标所有线程，返回 PC 落在 [lo,hi) 内的线程数。
// 这是"代码有没有真的开始执行"的硬证据 —— 不依赖目标写任何文件。
inline int threads_running_in_range(mach_port_t task, uint64_t lo, uint64_t hi, uint64_t* samplePc = nullptr) {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(task, &list, &count) != KERN_SUCCESS || !list) return 0;
    int n = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(list[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
            if (st.__pc >= lo && st.__pc < hi) {
                n++;
                if (samplePc && !*samplePc) *samplePc = (uint64_t)st.__pc;
            }
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));
    return n;
}


struct SavedThread {
    thread_t tid = MACH_PORT_NULL;
    arm_thread_state64_t state{};
    bool valid = false;
};

// 打印目标所有线程的状态，便于挑选劫持对象
inline void dump_threads(mach_port_t task) {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = task_threads(task, &list, &count);
    printf("[hijack] task_threads -> %d, 线程数=%u\n", kr, count);
    if (kr != KERN_SUCCESS || !list) return;

    for (mach_msg_type_number_t i = 0; i < count; i++) {
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        kern_return_t r = thread_get_state(list[i], ARM_THREAD_STATE64,
                                           (thread_state_t)&st, &cnt);
        thread_basic_info_data_t bi;
        mach_msg_type_number_t bcnt = THREAD_BASIC_INFO_COUNT;
        kern_return_t r2 = thread_info(list[i], THREAD_BASIC_INFO,
                                       (thread_info_t)&bi, &bcnt);
        printf("[hijack]   tid=%u state=%d pc=0x%llx sp=0x%llx x18=0x%llx run=%d\n",
               (unsigned)i, r,
               r == KERN_SUCCESS ? (unsigned long long)st.__pc : 0ULL,
               r == KERN_SUCCESS ? (unsigned long long)st.__sp : 0ULL,
               r == KERN_SUCCESS ? (unsigned long long)st.__x[18] : 0ULL,
               r2 == KERN_SUCCESS ? bi.run_state : -1);
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));
}

// 给目标分配一块栈
inline uintptr_t alloc_stack(mach_port_t task, size_t size) {
    vm_address_t base = 0;
    if (vm_allocate(task, &base, (vm_size_t)size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return 0;
    return (uintptr_t)base;
}

// 主流程：在目标里挑一个线程，暂停、改寄存器、恢复、等待、还原
inline bool hijack_call(mach_port_t task, uintptr_t fn, uint64_t x0,
                        int timeoutMs = 2000, int maxTries = 8) {
    if (!task || !fn) return false;

    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = task_threads(task, &list, &count);
    if (kr != KERN_SUCCESS || !list || count == 0) {
        printf("[hijack] task_threads 失败 (%d)\n", kr);
        return false;
    }
    printf("[hijack] 目标线程数=%u，入口=0x%llx\n", count, (unsigned long long)fn);

    uintptr_t stack = alloc_stack(task, 0x10000);
    if (!stack) { printf("[hijack] 栈分配失败\n"); return false; }
    uint64_t stackTop = (uint64_t)((stack + 0x10000) & ~0xFULL);

    bool done = false;
    int tried = 0;

    for (mach_msg_type_number_t i = 0; i < count && !done && tried < maxTries; i++) {
        SavedThread sv;
        sv.tid = list[i];

        // 备份
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(sv.tid, ARM_THREAD_STATE64,
                             (thread_state_t)&sv.state, &cnt) != KERN_SUCCESS) {
            printf("[hijack] tid=%u 读寄存器失败，跳过\n", (unsigned)i);
            continue;
        }
        // 注意：x18 是 iOS 保留寄存器，thread_get_state 永远读回 0，
        // 不能用它判断"有没有 TLS"。这里只排除明显不可用的线程。
        if (sv.state.__pc == 0) {
            printf("[hijack] tid=%u PC 为 0，跳过\n", (unsigned)i);
            continue;
        }
        sv.valid = true;
        tried++;

        // 暂停并改写
        if (thread_suspend(sv.tid) != KERN_SUCCESS) {
            printf("[hijack] tid=%u suspend 失败\n", (unsigned)i);
            continue;
        }

        arm_thread_state64_t st = sv.state;
        st.__pc   = (uint64_t)fn;
        st.__sp   = stackTop;
        st.__lr   = 0;
        st.__x[0] = x0;
        // x18 保留原值 —— 这正是劫持的意义

        kern_return_t w = thread_set_state(sv.tid, ARM_THREAD_STATE64,
                                           (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
        printf("[hijack] tid=%u set_state -> %d\n", (unsigned)i, w);
        if (w != KERN_SUCCESS) { thread_resume(sv.tid); continue; }

        if (thread_resume(sv.tid) != KERN_SUCCESS) {
            printf("[hijack] tid=%u resume 失败\n", (unsigned)i);
            continue;
        }

        // 等待：轮询 PC 是否已经离开入口（说明它跑进去了）
        int waited = 0;
        bool entered = false;
        while (waited < timeoutMs) {
            usleep(10 * 1000);
            waited += 10;
            arm_thread_state64_t now;
            mach_msg_type_number_t c2 = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(sv.tid, ARM_THREAD_STATE64,
                                 (thread_state_t)&now, &c2) != KERN_SUCCESS) break;
            // 入口第一件事是调 open/write，PC 会离开 fn 附近
            if (now.__pc < fn - 0x1000 || now.__pc > fn + 0x4000) { entered = true; break; }
        }
        printf("[hijack] tid=%u 等待 %d ms，entered=%d\n", (unsigned)i, waited, entered);

        // 还原
        thread_suspend(sv.tid);
        kern_return_t r = thread_set_state(sv.tid, ARM_THREAD_STATE64,
                                           (thread_state_t)&sv.state, ARM_THREAD_STATE64_COUNT);
        printf("[hijack] tid=%u 还原 -> %d\n", (unsigned)i, r);
        thread_resume(sv.tid);

        done = entered;
        if (!done) printf("[hijack] tid=%u 没进去，试下一个\n", (unsigned)i);
    }

    // 释放线程端口引用
    for (mach_msg_type_number_t i = 0; i < count; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));

    printf("[hijack] 结果=%s（尝试 %d 个线程）\n", done ? "成功" : "失败", tried);
    return done;
}

} // namespace hijack
