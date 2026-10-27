import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ============ 1) 日志改为无缓冲：闪退时也能留下日志 ============
hp = root + r"\host\main.mm"
h = open(hp, encoding="utf-8").read()

if "setvbuf" not in h:
    # 找到重定向函数里的 freopen，紧跟其后加无缓冲设置
    h = h.replace('    freopen(path, "a", stdout);',
                  '    freopen(path, "a", stdout);\n'
                  '    // 关键：默认是块缓冲，进程被系统杀掉时缓冲区里的日志会全部丢失。\n'
                  '    // 改成无缓冲后，任何一次 printf 都立刻落盘 —— 闪退也留得下现场。\n'
                  '    setvbuf(stdout, NULL, _IONBF, 0);')
    if "setvbuf" not in h:
        # 兜底：在 main 里设置
        h = h.replace('int main(int argc, char** argv) {',
                      'int main(int argc, char** argv) {\n    setvbuf(stdout, NULL, _IONBF, 0);')
    print("host unbuffered:", "setvbuf" in h)
open(hp, "w", encoding="utf-8", newline="\n").write(h)

# daemon 同样处理
dp = root + r"\daemon\main.mm"
d = open(dp, encoding="utf-8").read()
if "setvbuf" not in d:
    d = d.replace('int main(int argc, char** argv) {',
                  'int main(int argc, char** argv) {\n    setvbuf(stdout, NULL, _IONBF, 0);')
    open(dp, "w", encoding="utf-8", newline="\n").write(d)
print("daemon unbuffered:", "setvbuf" in d)

# ============ 2) ManualLoad：执行模式参数 + 禁止劫持调用者线程 ============
mp = root + r"\src\Inject\ManualLoad.h"
m = open(mp, encoding="utf-8").read()

m = m.replace("inline uintptr_t map_and_call(mach_port_t task, const char* dylibPath, const char* entrySym, const char* notePath = nullptr, bool hijackEntry = false) {",
              "// execMode: 0 = 短窗口劫持已有线程；1 = 只用 thread_create（自测用，绝不动别人线程）\n"
              "inline uintptr_t map_and_call(mach_port_t task, const char* dylibPath, const char* entrySym,\n"
              "                              const char* notePath = nullptr, bool hijackEntry = false,\n"
              "                              int execMode = 0) {")

# 入口执行块：按 execMode 分流
old_loop = '''            long probeVals[5] = {0, 0, 0, 0, 0};
            bool ranOk = false;
            for (int attempt = 0; attempt < 2 && !ranOk; attempt++) {
                hijack::hijack_quick(task, entry, (uint64_t)slot, 60, 3);'''
new_loop = '''            long probeVals[5] = {0, 0, 0, 0, 0};
            bool ranOk = false;

            if (execMode == 1) {
                // 自测模式：只新建线程，绝不碰任何已有线程
                printf("[mload] 执行模式=thread_create（不劫持任何已有线程）\\n");
                call_addr(task, entry, stackTop, (uint64_t)slot, tls);
                usleep(300 * 1000);
                if (slot && read_remote_slot(task, slot, probeVals) && probeVals[0] != 0) ranOk = true;
            } else for (int attempt = 0; attempt < 2 && !ranOk; attempt++) {
                hijack::hijack_quick(task, entry, (uint64_t)slot, 60, 3);'''
if old_loop in m:
    m = m.replace(old_loop, new_loop)
    print("execMode branch added")
else:
    print("!! entry loop anchor missing")

open(mp, "w", encoding="utf-8", newline="\n").write(m)

# ============ 3) 自测改为 execMode=1 ============
h2 = open(hp, encoding="utf-8").read()
h2 = h2.replace('mload::map_and_call(task, dylib.UTF8String, "arp_probe", nullptr, false);',
                'mload::map_and_call(task, dylib.UTF8String, "arp_probe", nullptr, false, 1);')
open(hp, "w", encoding="utf-8", newline="\n").write(h2)
print("self-test uses thread_create:", ", 1);" in h2)

# ============ 4) 审查脚本：新增两条不变量 ============
ap = root + r"\tools\audit.py"
a = open(ap, encoding="utf-8").read()
if "无缓冲" not in a:
    a = a.replace('''    if "entryRan" not in ml:''', '''    if "entryRan" not in ml:''')
    a = a.replace('''# ---------------- C. 逻辑不变量 ----------------''',
'''# ---------------- C. 逻辑不变量 ----------------
# 日志必须无缓冲：闪退时块缓冲会丢掉全部现场
host_src = read("host/main.mm")
if host_src and "setvbuf" not in host_src:
    FAIL.append("[诊断] host 未设置无缓冲 stdout，闪退时日志会丢失")
dmn_src = read("daemon/main.mm")
if dmn_src and "setvbuf" not in dmn_src:
    FAIL.append("[诊断] daemon 未设置无缓冲 stdout")
''')
    open(ap, "w", encoding="utf-8", newline="\n").write(a)
    print("audit unbuffered rule added")

# 括号自检
for f in (mp, hp, dp):
    src = open(f, encoding="utf-8").read()
    depth = 0
    for ln in src.split("\n"):
        tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
        for ch in tt:
            if ch == '{': depth += 1
            elif ch == '}': depth -= 1
    print(f.split("\\")[-1], "depth =", depth)
