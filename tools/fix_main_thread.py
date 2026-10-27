import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ---- 1) ThreadHijack：跳过 tid=0（通常是主线程），避免堵死 run loop ----
p = root + r"\src\Inject\ThreadHijack.h"
s = open(p, encoding="utf-8").read()

# hijack_call：从 i=1 开始
s = s.replace(
    "    for (mach_msg_type_number_t i = 0; i < count && !done && tried < maxTries; i++) {",
    "    // 从 i=1 开始：task_threads 的第 0 个通常是主线程，\n"
    "    // 劫持主线程等于堵死目标的 run loop，watchdog 会立刻杀掉进程。\n"
    "    for (mach_msg_type_number_t i = (count > 1 ? 1 : 0); i < count && !done && tried < maxTries; i++) {")

# probe_execution：同样从 i=1 开始
s = s.replace(
    "    for (mach_msg_type_number_t i = 0; i < count && !ok; i++) {",
    "    // 同样跳过主线程：探针会自旋，占住主线程会被 watchdog 干掉\n"
    "    for (mach_msg_type_number_t i = (count > 1 ? 1 : 0); i < count && !ok; i++) {")

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("ThreadHijack: skip tid=0 ->", "count > 1 ? 1 : 0" in s)

# ---- 2) 注入改回 thread_create 优先（新建专属线程），劫持只作兜底 ----
for path in (root + r"\daemon\main.mm", root + r"\host\main.mm"):
    t = open(path, encoding="utf-8").read()
    t = t.replace('"arp_note", note[0] ? note : nullptr, true)', '"arp_note", note[0] ? note : nullptr, false)')
    t = t.replace('"arp_note", note[0] ? note : nullptr, true)', '"arp_note", note[0] ? note : nullptr, false)')
    open(path, "w", encoding="utf-8", newline="\n").write(t)
    print("patched:", path.split("\\")[-1], "hijackEntry=false ->", ", false)" in t)

# ---- 3) 括号自检 ----
s = open(p, encoding="utf-8").read()
depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("ThreadHijack depth =", depth)
