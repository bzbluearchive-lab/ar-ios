import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

snippet = r'''
                // 注入后回读面包屑文件：让 daemon 日志自带"代码有没有跑起来"的结论，
                // 不用再去翻 ar_full_log.txt。
                sleep(2);
                FILE* nf = note[0] ? fopen(note, "r") : nullptr;
                if (nf) {
                    char buf[512] = {0};
                    size_t got = fread(buf, 1, sizeof(buf) - 1, nf);
                    fclose(nf);
                    if (got > 0) {
                        L("面包屑内容(%zu 字节):", got);
                        char* line = strtok(buf, "\n");
                        while (line) {
                            L("  | %s", line);
                            line = strtok(nullptr, "\n");
                        }
                    } else {
                        L("面包屑文件存在但为空 —— 说明入口代码没执行到写文件那一步");
                    }
                } else {
                    L("面包屑文件不存在 —— 入口代码没有执行（或 open 被拒）");
                }
'''

for path, anchor in [
    (root + r"\daemon\main.mm", '                    uintptr_t rb = mload::map_and_call(task, payload, "arp_note", note[0] ? note : nullptr, false);'),
]:
    t = open(path, encoding="utf-8").read()
    if "面包屑内容" in t:
        print("already patched:", path)
        continue
    if anchor in t:
        t = t.replace(anchor, anchor + snippet)
        open(path, "w", encoding="utf-8", newline="\n").write(t)
        print("patched:", path.split("\\")[-1])
    else:
        print("!! anchor missing in", path)

# 括号自检
s = open(root + r"\daemon\main.mm", encoding="utf-8").read()
depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("daemon depth =", depth)
