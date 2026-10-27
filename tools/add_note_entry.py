import re

# ---- 1) payload 增加 arp_note：只有裸 syscall + 自旋，完全不碰 ObjC ----
p1 = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\payload_manual\main.mm"
s = open(p1, encoding="utf-8").read()

add = '''
// 最小入口：只做裸 syscall 面包屑，然后自旋。
//   - 不碰 ObjC、不碰 bind、不碰 TLS
//   - 用来单独回答一个问题：从"手动映射进来的匿名可执行页"里
//     直接 svc #0x80 能不能成功？
//   能写出 note_ok 就说明整条执行链（映射→执行→进内核）都通，
//   剩下的问题只会在 ObjC 层。
extern "C" __attribute__((visibility("default")))
void arp_note(const char* notePath) {
    rawNote(notePath, "note_ok\\n");
    volatile unsigned long n = 0;
    for (;;) { n++; }
}

'''
if "arp_note" not in s:
    s = s.replace("// 纯自旋：不调用任何外部函数，用来验证\"映射进来的代码能否被执行\"", add + "// 纯自旋：不调用任何外部函数，用来验证\"映射进来的代码能否被执行\"")
    open(p1, "w", encoding="utf-8", newline="\n").write(s)
    print("payload: arp_note added")
else:
    print("payload: arp_note already there")

# ---- 2) daemon 与 host 都改成调用 arp_note（安全测试入口）----
for path, old, new in [
    (r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\daemon\main.mm",
     'mload::map_and_call(task, payload, "arp_entry", note[0] ? note : nullptr, true)',
     'mload::map_and_call(task, payload, "arp_note", note[0] ? note : nullptr, true)'),
    (r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\host\main.mm",
     'mload::map_and_call(task, dylib.UTF8String, "arp_entry", note[0] ? note : nullptr, true)',
     'mload::map_and_call(task, dylib.UTF8String, "arp_note", note[0] ? note : nullptr, true)'),
]:
    t = open(path, encoding="utf-8").read()
    if old in t:
        t = t.replace(old, new)
        open(path, "w", encoding="utf-8", newline="\n").write(t)
        print("patched:", path.split("\\")[-2] + "\\" + path.split("\\")[-1])
    else:
        print("!! 未匹配:", path)

# ---- 3) 括号自检 ----
s = open(p1, encoding="utf-8").read()
depth = 0
for ln in s.split("\n"):
    t = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in t:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("payload depth =", depth)
