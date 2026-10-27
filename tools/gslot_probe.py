import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ---------------- payload：结果写进自身镜像里的全局 g_slot，不依赖任何参数 ----------------
p1 = root + r"\payload_manual\main.mm"
s = open(p1, encoding="utf-8").read()

new_probe = r'''
// 结果槽放在 payload 镜像自己的 __DATA 里。
// 注入方通过符号 g_slot 拿到它的地址再直接读，
// 这样就完全不需要通过寄存器传指针 ——
// 之前用 x1 传槽地址时，代码连第一个字节都没写，怀疑参数没进去。
#define AR_MAGIC_ENTERED 0xC0FFEE11L
#define AR_MAGIC_DONE    0xDEADBEEFL

extern "C" __attribute__((visibility("default")))
volatile long g_slot[8];

extern "C" __attribute__((visibility("default")))
void arp_probe(const char* notePath) {
    g_slot[0] = AR_MAGIC_ENTERED;      // 只要这行执行，槽里就有值

    long fd = -1;
    if (notePath) fd = ar_sys3(5 /*open*/, (long)notePath, 0x209, 0644);
    g_slot[1] = fd;

    if (fd >= 0) {
        g_slot[2] = ar_sys3(4 /*write*/, fd, (long)"ok\n", 3);
        ar_sys3(6 /*close*/, fd, 0, 0);
    }
    g_slot[3] = AR_MAGIC_DONE;

    volatile unsigned long n = 0;
    for (;;) { n++; }
}

'''
if "g_slot" not in s:
    s = s.replace("// 纯自旋：不调用任何外部函数，用来验证\"映射进来的代码能否被执行\"",
                  new_probe + "// 纯自旋：不调用任何外部函数，用来验证\"映射进来的代码能否被执行\"")
    open(p1, "w", encoding="utf-8", newline="\n").write(s)
    print("payload: g_slot probe added")

# ---------------- loader：读 g_slot 符号地址，劫持后从那里读结果 ----------------
p3 = root + r"\src\Inject\ManualLoad.h"
m = open(p3, encoding="utf-8").read()

# 用 g_slot 的符号地址替代 vm_allocate 出来的槽
old_alloc = '''            // 结果槽：payload 把每一步的执行结果写在这里，我们直接读回，
            // 完全不依赖目标沙盒里的文件系统。
            if (vm_allocate(task, &slot, 0x1000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) slot = 0;
            printf("[mload] 结果槽=0x%llx\\n", (unsigned long long)slot);'''
new_alloc = '''            // 结果槽改用 payload 镜像里的全局符号 g_slot，
            // 地址由符号表算出，不经过任何寄存器传参。
            slot = resolve_sym(img, "g_slot");
            printf("[mload] 结果槽(g_slot)=0x%llx\\n", (unsigned long long)slot);'''
if old_alloc in m:
    m = m.replace(old_alloc, new_alloc)
    print("loader: g_slot used")
else:
    print("!! alloc anchor missing")

# 劫持调用不再传 x1
m = m.replace('if (!hijack::hijack_call(task, entry, (uint64_t)noteAddr, 900, 3, (uint64_t)slot)) {',
              'if (!hijack::hijack_call(task, entry, (uint64_t)noteAddr, 900, 3, 0)) {')
m = m.replace('call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls, (uint64_t)slot);',
              'call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls, 0);')

open(p3, "w", encoding="utf-8", newline="\n").write(m)

depth = 0
for ln in m.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("ManualLoad depth =", depth)
print("g_slot resolve:", 'resolve_sym(img, "g_slot")' in m)
