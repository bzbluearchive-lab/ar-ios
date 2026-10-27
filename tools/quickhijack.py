import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ---- ThreadHijack：新增"短窗口劫持" ----
p = root + r"\src\Inject\ThreadHijack.h"
s = open(p, encoding="utf-8").read()

quick = r'''
// 短窗口劫持：借一个非主线程，把 PC 指到 fn，只冻结 holdMs 毫秒就还原。
//
// 为什么必须"短"：冻结游戏线程会让它卡住，时间长了系统 watchdog 直接杀进程
// （实测 280ms 以上就危险）。而写几个内存值只需要几微秒，
// 所以 20~30ms 足够让目标代码跑完，同时不会让游戏卡死。
//
// 不再用"PC 有没有离开函数范围"判断成功与否 —— 那套判据对自循环代码永远失效。
// 成败由调用方读结果槽决定。
inline bool hijack_quick(mach_port_t task, uintptr_t fn, uint64_t x0,
                         int holdMs = 25, int maxTries = 4, uint64_t* triedOut = nullptr) {
    if (!task || !fn) return false;

    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(task, &list, &count) != KERN_SUCCESS || !list) return false;

    uintptr_t stack = alloc_stack(task, 0x8000);
    if (!stack) {
        for (mach_msg_type_number_t i = 0; i < count; i++) mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));
        return false;
    }
    uint64_t stackTop = (uint64_t)((stack + 0x8000) & ~0xFULL);

    // 从末尾往前挑（第 0 个通常是主线程）
    int floorIdx = (count > 1) ? 1 : 0;
    int tried = 0;
    bool ok = false;

    for (int idx = (int)count - 1; idx >= floorIdx && tried < maxTries; idx--) {
        thread_t tid = list[idx];

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

        kern_return_t w = thread_set_state(tid, ARM_THREAD_STATE64,
                                           (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
        if (w != KERN_SUCCESS) { thread_resume(tid); continue; }

        thread_resume(tid);
        tried++;
        if (triedOut) *triedOut = (uint64_t)tried;

        // 只放它跑 holdMs 毫秒
        usleep(holdMs * 1000);

        thread_suspend(tid);
        kern_return_t r = thread_set_state(tid, ARM_THREAD_STATE64,
                                           (thread_state_t)&sv, ARM_THREAD_STATE64_COUNT);
        if (r != KERN_SUCCESS) {
            usleep(3000);
            r = thread_set_state(tid, ARM_THREAD_STATE64,
                                 (thread_state_t)&sv, ARM_THREAD_STATE64_COUNT);
        }
        thread_resume(tid);
        printf("[hijack] 短劫持 tid=%d 冻结 %d ms 还原->%d\n", idx, holdMs, r);
        ok = (r == KERN_SUCCESS);
        if (ok) break;
    }

    for (mach_msg_type_number_t i = 0; i < count; i++) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_t));
    printf("[hijack] 短劫持结束: tried=%d ok=%d\n", tried, ok);
    return ok;
}

'''
if "hijack_quick" not in s:
    s = s.replace("// 主流程：在目标里挑一个线程，暂停、改寄存器、恢复、等待、还原", quick + "// 主流程：在目标里挑一个线程，暂停、改寄存器、恢复、等待、还原")
    open(p, "w", encoding="utf-8", newline="\n").write(s)
    print("hijack_quick added")

# ---- ManualLoad：改成短窗口劫持 + 用结果槽判断，失败就下一个线程 ----
p2 = root + r"\src\Inject\ManualLoad.h"
m = open(p2, encoding="utf-8").read()

old = '''            if (hijackEntry) {
                if (!hijack::hijack_call(task, entry, (uint64_t)slot, 900, 3, 0)) {
                    printf("[mload] 劫持失败，回退到新建线程\\n");
                    call_addr(task, entry, stackTop, (uint64_t)slot, tls);
                }
            } else {
                call_addr(task, entry, stackTop, (uint64_t)slot, tls);
            }'''

new = '''            // 短窗口劫持：每个候选线程只冻结 25ms。
            // 每试一个线程就读一次结果槽，成功就停 —— 这样平均冻结时间最短。
            long probeVals[5] = {0, 0, 0, 0, 0};
            bool ranOk = false;
            for (int attempt = 0; attempt < 3 && !ranOk; attempt++) {
                hijack::hijack_quick(task, entry, (uint64_t)slot, 25, 1);
                usleep(150 * 1000);
                if (slot && read_remote_slot(task, slot, probeVals) && probeVals[0] != 0) {
                    ranOk = true;
                    printf("[mload] 第 %d 次短劫持后结果槽有值，停止尝试\\n", attempt + 1);
                }
            }
            if (!ranOk) {
                printf("[mload] 短劫持未能让入口执行；不再回退新建线程（已验证不执行）\\n");
            }'''

if old in m:
    m = m.replace(old, new)
    open(p2, "w", encoding="utf-8", newline="\n").write(m)
    print("ManualLoad: short hijack path")
else:
    print("!! entry-block anchor missing")

depth = 0
for ln in m.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("ManualLoad depth =", depth)
