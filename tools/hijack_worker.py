import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ---- 1) hijack_call：从线程表末尾往前挑（末尾通常是工作线程），跳过主线程 ----
p = root + r"\src\Inject\ThreadHijack.h"
s = open(p, encoding="utf-8").read()

old_loop = "    // 从 i=1 开始：task_threads 的第 0 个通常是主线程，\n    // 劫持主线程等于堵死目标的 run loop，watchdog 会立刻杀掉进程。\n    for (mach_msg_type_number_t i = (count > 1 ? 1 : 0); i < count && !done && tried < maxTries; i++) {"
new_loop = ("    // 从线程表**末尾**往前挑：第 0 个通常是主线程（劫持它等于堵死 run loop，\n"
            "    // watchdog 会立刻杀进程），末尾一般是工作线程，被短暂占用影响最小。\n"
            "    for (int idx = (int)count - 1; idx >= 1 && !done && tried < maxTries; idx--) {\n"
            "        mach_msg_type_number_t i = (mach_msg_type_number_t)idx;")
if old_loop in s:
    s = s.replace(old_loop, new_loop)
    print("hijack loop: reversed")
else:
    print("!! hijack loop anchor missing")

open(p, "w", encoding="utf-8", newline="\n").write(s)

# ---- 2) 入口改回"劫持"，并放宽等待时间让 svc 有时间完成 ----
for path in (root + r"\daemon\main.mm", root + r"\host\main.mm"):
    t = open(path, encoding="utf-8").read()
    t = t.replace('"arp_note", note[0] ? note : nullptr, false)', '"arp_note", note[0] ? note : nullptr, true)')
    open(path, "w", encoding="utf-8", newline="\n").write(t)
    print("patched entry=hijack:", path.split("\\")[-1])

# ---- 3) 括号自检 ----
s = open(p, encoding="utf-8").read()
depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("ThreadHijack depth =", depth)
print("has reversed loop:", "int idx = (int)count - 1" in s)
