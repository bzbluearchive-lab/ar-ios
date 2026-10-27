import re

p = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\src\Inject\ManualLoad.h"
s = open(p, encoding="utf-8").read()

start_marker = "    if (entry) {"
end_marker = '    } else {\n        printf("[mload] 没找到入口符号'

i = s.index(start_marker)
j = s.index(end_marker)

new_block = r'''    if (entry) {
        // ---- 1) 先做安全的自旋探针：只验证"映射进来的代码能否被执行" ----
        //     arp_spin 不调用任何外部函数，所以它失败不会破坏目标；
        //     探针不过就绝不调用 arp_entry，避免为了解一个未知问题把游戏搞崩。
        uintptr_t spin = resolve_sym(img, "arp_spin");
        bool spinOk = false;
        if (spin) {
            printf("[mload] 自旋探针地址=0x%llx\n", (unsigned long long)spin);
            spinOk = hijack::probe_execution(task, spin);
            printf("[mload] 自旋探针结果=%s\n", spinOk ? "代码可执行" : "代码不可执行");
        } else {
            printf("[mload] 没找到 arp_spin 符号，无法确认可执行性；跳过入口调用以策安全\n");
        }

        // ---- 2) 回读入口首指令（确认内容写对了）----
        uint32_t insn = 0;
        vm_size_t got2 = 0;
        if (vm_read_overwrite(task, (vm_address_t)entry, 4, (vm_address_t)&insn, &got2) == KERN_SUCCESS && got2 == 4) {
            printf("[mload] 入口首指令 = 0x%08x%s\n", insn,
                   (insn == 0 || insn == 0xFFFFFFFF) ? "  << 全 0/全 F，代码没写进去！" : "");
        }

        // ---- 3) 只有探针确认可执行，才真正调用 arp_entry ----
        if (spinOk) {
            uint64_t tls = grab_tls_from_target(task);
            uintptr_t noteAddr = notePath ? write_string(task, notePath) : 0;
            if (noteAddr)
                printf("[mload] 面包屑路径已写入目标: 0x%llx (%s)\n",
                       (unsigned long long)noteAddr, notePath);
            printf("[mload] 调用入口 %s @ 0x%llx (x0=0x%llx) 方式=%s\n", entrySym,
                   (unsigned long long)entry, (unsigned long long)noteAddr,
                   hijackEntry ? "劫持已有线程" : "新建线程");
            if (hijackEntry) {
                if (!hijack::hijack_call(task, entry, (uint64_t)noteAddr)) {
                    printf("[mload] 劫持失败，回退到新建线程\n");
                    call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls);
                }
            } else {
                call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls);
            }
        } else {
            printf("[mload] 跳过 arp_entry 调用（自旋探针未通过）\n");
        }
'''

s = s[:i] + new_block + s[j:]
open(p, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    t = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in t:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
print("depth =", depth)
print("spin-first:", "先做安全的自旋探针" in s)
