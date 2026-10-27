import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ---------------- 1) payload：结果写进调用方给的内存槽，不依赖文件 ----------------
p1 = root + r"\payload_manual\main.mm"
s = open(p1, encoding="utf-8").read()

new_probe = r'''
// 结果槽协议的魔数
#define AR_MAGIC_ENTERED 0xC0FFEE11L
#define AR_MAGIC_DONE    0xDEADBEEFL
#define AR_MAGIC_OPENFAIL 0x0BAD0PENL

// 核心观测入口：把每一步结果写进调用方（注入方）分配的内存槽，
//   slot[0] = AR_MAGIC_ENTERED  -> 代码真的开始执行了
//   slot[1] = open() 的返回值   -> 负数是错误码
//   slot[2] = write() 的返回值
//   slot[3] = AR_MAGIC_DONE     -> 全部走完
//   slot[4] = 文件描述符用于 close 之后是否还有效（0 表示已关闭）
// 这样完全不依赖沙盒里的文件系统，注入方 vm_read 就能看到结论。
extern "C" __attribute__((visibility("default")))
void arp_probe(const char* notePath, volatile long* slot) {
    if (slot) slot[0] = AR_MAGIC_ENTERED;

    long fd = -1;
    if (notePath) {
        fd = ar_sys3(5 /*open*/, (long)notePath, 0x209, 0644);
    }
    if (slot) slot[1] = fd;

    long wr = -1;
    if (fd >= 0) {
        wr = ar_sys3(4 /*write*/, fd, (long)"ok\n", 3);
        ar_sys3(6 /*close*/, fd, 0, 0);
    }
    if (slot) {
        slot[2] = wr;
        slot[3] = AR_MAGIC_DONE;
        slot[4] = 0;
    }

    volatile unsigned long n = 0;
    for (;;) { n++; }
}

'''
if "arp_probe" not in s:
    s = s.replace("// 纯自旋：不调用任何外部函数，用来验证\"映射进来的代码能否被执行\"",
                  new_probe + "// 纯自旋：不调用任何外部函数，用来验证\"映射进来的代码能否被执行\"")
    open(p1, "w", encoding="utf-8", newline="\n").write(s)
    print("payload: arp_probe added")
else:
    print("payload: arp_probe exists")

# ---------------- 2) ThreadHijack：支持传 x1 ----------------
p2 = root + r"\src\Inject\ThreadHijack.h"
t = open(p2, encoding="utf-8").read()
t = t.replace(
    "inline bool hijack_call(mach_port_t task, uintptr_t fn, uint64_t x0,\n                        int timeoutMs = 600, int maxTries = 3) {",
    "inline bool hijack_call(mach_port_t task, uintptr_t fn, uint64_t x0,\n                        int timeoutMs = 600, int maxTries = 3, uint64_t x1 = 0) {")
t = t.replace("        st.__x[0] = x0;\n        // x18 保留原值 —— 这正是劫持的意义",
              "        st.__x[0] = x0;\n        st.__x[1] = x1;\n        // x18 保留原值 —— 这正是劫持的意义")
open(p2, "w", encoding="utf-8", newline="\n").write(t)
print("hijack x1 param:", "uint64_t x1 = 0" in t)

# ---------------- 3) ManualLoad：分配槽、传 x0/x1、劫持后回读槽 ----------------
p3 = root + r"\src\Inject\ManualLoad.h"
m = open(p3, encoding="utf-8").read()

# call_addr 也要支持 x1
m = m.replace("static inline kern_return_t call_addr(mach_port_t task, uintptr_t fn, uintptr_t stackTop,\n                                      uint64_t x0 = 0, uint64_t x18 = 0) {",
              "static inline kern_return_t call_addr(mach_port_t task, uintptr_t fn, uintptr_t stackTop,\n                                      uint64_t x0 = 0, uint64_t x18 = 0, uint64_t x1 = 0) {")
m = m.replace("    st.__x[0]  = x0;\n    st.__x[18] = x18;          // TLS",
              "    st.__x[0]  = x0;\n    st.__x[1]  = x1;\n    st.__x[18] = x18;          // TLS")

# 在 entry 调用处加结果槽
old = '''        if (spinOk) {
            uint64_t tls = grab_tls_from_target(task);
            uintptr_t noteAddr = notePath ? write_string(task, notePath) : 0;'''
new = '''        if (spinOk) {
            uint64_t tls = grab_tls_from_target(task);
            uintptr_t noteAddr = notePath ? write_string(task, notePath) : 0;

            // 结果槽：payload 把每一步的执行结果写在这里，我们直接读回，
            // 完全不依赖目标沙盒里的文件系统。
            vm_address_t slot = 0;
            if (vm_allocate(task, &slot, 0x1000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) slot = 0;
            printf("[mload] 结果槽=0x%llx\\n", (unsigned long long)slot);'''
m = m.replace(old, new)

# 劫持/新建线程调用都带上 x1
m = m.replace('if (!hijack::hijack_call(task, entry, (uint64_t)noteAddr)) {',
              'if (!hijack::hijack_call(task, entry, (uint64_t)noteAddr, 900, 3, (uint64_t)slot)) {')
m = m.replace('                call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls);\n            }\n        } else {',
              '                call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls, (uint64_t)slot);\n            }\n        } else {')
m = m.replace('        } else {\n            call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls);\n        }',
              '        } else {\n            call_addr(task, entry, stackTop, (uint64_t)noteAddr, tls, (uint64_t)slot);\n        }')

# 调用后回读槽
old_tail = '''        } else {
            printf("[mload] 跳过 arp_entry 调用（自旋探针未通过）\\n");
        }'''
new_tail = '''        } else {
            printf("[mload] 跳过入口调用（自旋探针未通过）\\n");
        }

        // ---- 4) 回读结果槽：这是"代码有没有执行、open 有没有成功"的硬证据 ----
        if (slot) {
            long vals[5] = {0, 0, 0, 0, 0};
            for (int i = 0; i < 8; i++) {
                usleep(150 * 1000);
                if (!read_remote_slot(task, slot, vals)) continue;
                if (vals[0] != 0) break;
            }
            printf("[mload] 结果槽: entered=0x%lx open=%ld write=%ld done=0x%lx\\n",
                   vals[0], vals[1], vals[2], vals[3]);
            if (vals[0] == 0) {
                printf("[mload] 槽仍是空的 -> 入口代码根本没开始执行\\n");
            } else {
                printf("[mload] 代码已执行；open 返回 %ld%s\\n", vals[1],
                       vals[1] < 0 ? "（失败：沙盒拒绝或路径不可写）" : "（成功）");
            }
        }'''
m = m.replace(old_tail, new_tail)

# 加一个读槽助手
helper = '''
// 从目标内存读回结果槽（5 个 long）
static inline bool read_remote_slot(mach_port_t task, uint64_t addr, long out[5]) {
    vm_size_t got = 0;
    if (vm_read_overwrite(task, (vm_address_t)addr, sizeof(long) * 5,
                          (vm_address_t)out, &got) != KERN_SUCCESS) return false;
    return got == sizeof(long) * 5;
}

'''
m = m.replace("// ---------------- 主流程：把 dylib 手动映射进目标并调用入口 ----------------",
              helper + "// ---------------- 主流程：把 dylib 手动映射进目标并调用入口 ----------------")

open(p3, "w", encoding="utf-8", newline="\n").write(m)

depth = 0
for ln in m.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("ManualLoad depth =", depth)
print("slot read:", "结果槽: entered" in m)
