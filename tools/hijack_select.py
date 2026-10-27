import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
p = root + r"\src\Inject\ThreadHijack.h"
s = open(p, encoding="utf-8").read()

start = s.find("// 短窗口劫持：借一个非主线程")
end = s.find("// 主流程：在目标里挑一个线程")
if start == -1 or end == -1:
    print("!! markers not found", start, end)
    raise SystemExit(1)

new_fn = r'''// 短窗口劫持（按"线程是否真的在跑"精选目标）
//
// 上一版的问题：从线程表末尾随便挑 → 挑中的往往是阻塞在信号量/kevent 上的空闲线程。
// 对这种线程改 PC 是没用的：它要等系统调用返回才可能执行，25ms 内根本轮不到。
//
// 这一版做了三件事：
//   1. 用 thread_info 过滤：只挑 run_state==TH_STATE_RUNNING 且 cpu_usage>0 的线程
//      （真正在跑 CPU 的线程，改 PC 之后立刻就会执行我们的代码）
//   2. 冻结 holdMs 后回读 PC：若 PC 还停在入口地址 → 说明根本没执行，
//      立刻还原并换下一个（不再靠"猜"）
//   3. 还原失败立刻停手：那意味着线程状态已经坏了，继续动别的线程只会更糟
inline bool hijack_quick(mach_port_t task, uintptr_t fn, uint64_t x0,
                         int holdMs = 60, int maxTries = 6, int* ranOut = nullptr) {
    if (ranOut) *ranOut = 0;
    if (!task || !fn) return false;

    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(task, &list, &count) != KERN_SUCCESS || !list) return false;

    struct Cand { int idx; integer_t cpu; thread_t tid; };
    Cand cands[64];
    int nc = 0;

    // 收集候选：跳过主线程（idx 0），只留 RUNNING 且 CPU 用量 > 0 的
    for (mach_msg_type_number_t i = 1; i < count && nc < 64; i++) {
        thread_basic_info_data_t bi;
        mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
        if (thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) != KERN_SUCCESS) continue;
        if (bi.run_state != TH_STATE_RUNNING) continue;
        if (bi.cpu_usage <= 0) continue;
        cands[nc++] = { (int)i, bi.cpu_usage, list[i] };
    }
    printf("[hijack] 线程总数=%u 可安全借用的运行中线程=%d\n", count, nc);

    // 按 CPU 用量从高到低排序：最活跃的线程最可能立刻执行
    for (int a = 1; a < nc; a++) {
        Cand key = cands[a];
        int b = a - 1;
        while (b >= 0 && cands[b].cpu < key.cpu) { cands[b + 1] = cands[b]; b--; }
        cands[b + 1] = key;
    }

    uintptr_t stack = alloc_stack(task, 0x8000);
    if (!stack) {
        for (mach_msg_type_number_t i = 0; i < count; i++) mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));
        return false;
    }
    uint64_t stackTop = (uint64_t)((stack + 0x8000) & ~0xFULL);

    bool anyRan = false;
    bool fatal = false;
    int tried = 0;

    for (int c = 0; c < nc && tried < maxTries && !anyRan && !fatal; c++) {
        thread_t tid = cands[c].tid;

        arm_thread_state64_t sv;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(tid, ARM_THREAD_STATE64, (thread_state_t)&sv, &cnt) != KERN_SUCCESS) continue;
        if (sv.__pc == 0) continue;

        if (thread_suspend(tid) != KERN_SUCCESS) continue;

        arm_thread_state64_t st = sv;
        st.__pc = (uint64_t)fn;
        st.__sp = stackTop;
        st.__lr = 0;
        st.__x[0] = x0;

        if (thread_set_state(tid, ARM_THREAD_STATE64, (thread_state_t)&st,
                             ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS) {
            thread_resume(tid);
            continue;
        }
        thread_resume(tid);
        tried++;

        usleep(holdMs * 1000);

        // 判据：PC 是否离开了入口地址。还停在入口 = 这一下没执行。
        arm_thread_state64_t now;
        mach_msg_type_number_t c2 = ARM_THREAD_STATE64_COUNT;
        bool ranThis = false;
        if (thread_get_state(tid, ARM_THREAD_STATE64, (thread_state_t)&now, &c2) == KERN_SUCCESS) {
            ranThis = (now.__pc != (uint64_t)fn);
        }

        thread_suspend(tid);
        kern_return_t r = thread_set_state(tid, ARM_THREAD_STATE64,
                                           (thread_state_t)&sv, ARM_THREAD_STATE64_COUNT);
        if (r != KERN_SUCCESS) {
            usleep(3000);
            r = thread_set_state(tid, ARM_THREAD_STATE64,
                                 (thread_state_t)&sv, ARM_THREAD_STATE64_COUNT);
        }
        thread_resume(tid);

        printf("[hijack] tid=%d cpu=%d 冻结%dms 执行=%d 还原->%d\n",
               cands[c].idx, cands[c].cpu, holdMs, ranThis, r);

        if (r != KERN_SUCCESS) {
            printf("[hijack] 还原失败，立刻停止（继续操作别的线程会让目标更危险）\n");
            fatal = true;
            break;
        }
        if (ranThis) anyRan = true;
    }

    for (mach_msg_type_number_t i = 0; i < count; i++) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));
    vm_deallocate(task, (vm_address_t)stack, 0x8000);

    if (ranOut) *ranOut = anyRan ? 1 : 0;
    printf("[hijack] 短劫持结束: tried=%d ran=%d fatal=%d\n", tried, anyRan, fatal);
    return anyRan;
}

'''

s = s[:start] + new_fn + s[end:]
open(p, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
print("ThreadHijack depth =", depth)
print("has run_state filter:", "TH_STATE_RUNNING" in s)
print("has pc-check:", "ranThis = (now.__pc != (uint64_t)fn)" in s)
