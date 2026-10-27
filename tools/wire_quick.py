import re

p = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\src\Inject\ManualLoad.h"
lines = open(p, encoding="utf-8").read().split("\n")

# 替换 530..540 行（1-based）：printf 显示 x0 改为槽，并把入口执行改成"短窗口劫持"
new_block = r'''            printf("[mload] 调用入口 %s @ 0x%llx (x0=槽 0x%llx) 方式=%s\n", entrySym,
                   (unsigned long long)entry, (unsigned long long)slot,
                   hijackEntry ? "短窗口劫持(25ms)" : "新建线程");

            // 短窗口劫持：每次只冻结一个游戏线程 25ms。
            // 每试一次就读结果槽，有值就停 —— 平均冻结时间最短，
            // 既能让代码跑完，又不会让游戏卡到被 watchdog 杀掉。
            long probeVals[5] = {0, 0, 0, 0, 0};
            bool ranOk = false;
            for (int attempt = 0; attempt < 3 && !ranOk; attempt++) {
                hijack::hijack_quick(task, entry, (uint64_t)slot, 25, 1);
                usleep(150 * 1000);
                if (slot && read_remote_slot(task, slot, probeVals) && probeVals[0] != 0) {
                    ranOk = true;
                    printf("[mload] 第 %d 次短劫持后结果槽有值 -> 代码已执行\n", attempt + 1);
                } else {
                    printf("[mload] 第 %d 次短劫持后结果槽仍为空\n", attempt + 1);
                }
            }
            if (!ranOk) {
                printf("[mload] 短劫持未能让入口执行（不再回退新建线程：已验证不执行）\n");
            }'''.split("\n")

out = lines[:529] + new_block + lines[540:]
s = "\n".join(out)
open(p, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
print("depth =", depth)
print("short hijack wired:", "hijack::hijack_quick(task, entry" in s)
print("no thread_create fallback:", "call_addr(task, entry" not in s)
