import re

P = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\src\Kernel\KrwProc.h"
src = open(P, encoding="utf-8").read()

# ---------- 1) Ctx 加 current_proc ----------
old_ctx = """struct Ctx {
    uint64_t kfd;          // kfdg_open() 的句柄；0 = 不可用
    uint64_t kernel_proc;  // pid 0 那条 struct proc 的内核地址（kfdg_kernel_proc）
};"""
new_ctx = """struct Ctx {
    uint64_t kfd;          // kfdg_open() 的句柄；0 = 不可用
    uint64_t kernel_proc;  // pid 0 那条 struct proc 的内核地址（kfdg_kernel_proc）
    // ★ 关键：kernel_proc 是 kernel_task 的 proc，**不在 allproc 链表上**（实测 le_next=0），
    //   所以遍历链表必须从 current_proc 出发（它一定在链上）。
    uint64_t current_proc; // 本进程 struct proc 的内核地址（kfdg_current_proc）
};"""
assert old_ctx in src, "Ctx 锚点没匹配"
src = src.replace(old_ctx, new_ctx)

# ---------- 2) init 里取 current_proc ----------
old_init = """    c->kfd = kfdHandle;
    c->kernel_proc = kp;"""
new_init = """    uint64_t cp = kfdg_current_proc(kfdHandle);
    if (cp == 0) {
        printf("[krw] init 失败：kfdg_current_proc 返回 0（拿不到本进程 proc，无法遍历 p_list）\\n");
        return false;
    }
    if (!looks_like_kva(cp) || ((cp & 0x7ull) != 0)) {
        printf("[krw] init 失败：current_proc=0x%llx 不像内核 VA 或不对齐\\n",
               (unsigned long long)cp);
        return false;
    }
    printf("[krw] current_proc = 0x%llx（p_list 遍历起点）\\n", (unsigned long long)cp);

    c->kfd = kfdHandle;
    c->kernel_proc = kp;
    c->current_proc = cp;"""
assert old_init in src, "init 锚点没匹配"
src = src.replace(old_init, new_init)

# ---------- 3) 替换整个 find_proc ----------
start_marker = "inline uint64_t find_proc(const Ctx& c, int pid)"
i = src.index(start_marker)
# 找匹配的右花括号
j = src.index("{", i)
depth = 0
k = j
while k < len(src):
    if src[k] == '{':
        depth += 1
    elif src[k] == '}':
        depth -= 1
        if depth == 0:
            break
    k += 1
end = k + 1
assert depth == 0, "花括号没配平"

new_fn = r'''// 单向扫描：从 start 出发，forward=true 走 le_next、false 走 le_prev。
// LIST_ENTRY 语义：le_prev 存的是"前一个元素的 le_next 字段地址"；
// 因为 p_list 是 struct proc 的第一个成员（偏移 0），所以上一节点地址 == le_prev 的值。
inline uint64_t scan_dir(const Ctx& c, uint64_t start, int pid, bool forward, const char* tag)
{
    uint64_t cur = start;
    const uint64_t off = forward ? kProcPListLeNext : kProcPListLePrev;
    const char* fname = forward ? "le_next" : "le_prev";

    for (uint64_t step = 0; step < kMaxProcScan; step++) {
        uint64_t w = 0;
        if (!kread64(c, cur + kProcPid, &w)) {
            printf("[krw] scan(%s) 停止：读 p_pid 失败，第 %llu 个节点 proc=0x%llx（kaddr=0x%llx）\n",
                   tag, (unsigned long long)step, (unsigned long long)cur,
                   (unsigned long long)(cur + kProcPid));
            return 0;
        }
        int32_t np = (int32_t)(uint32_t)(w & 0xffffffffull);
        if (np == pid) {
            printf("[krw] scan(%s) 命中：pid=%d proc=0x%llx（第 %llu 个节点）\n",
                   tag, pid, (unsigned long long)cur, (unsigned long long)(step + 1ull));
            return cur;
        }

        uint64_t raw = 0;
        if (!kread64(c, cur + off, &raw)) {
            printf("[krw] scan(%s) 停止：读 %s 失败，第 %llu 个节点 proc=0x%llx\n",
                   tag, fname, (unsigned long long)step, (unsigned long long)cur);
            return 0;
        }
        if (raw == 0) {
            printf("[krw] scan(%s) 到链头/链尾：第 %llu 个节点 proc=0x%llx 的 %s=0\n",
                   tag, (unsigned long long)step, (unsigned long long)cur, fname);
            return 0;
        }
        uint64_t nxt = unsign_kaddr(raw);
        if ((nxt & 0x7ull) != 0) {
            printf("[krw] scan(%s) 停止：%s raw=0x%llx 归一后=0x%llx 不对齐（偏移或 PAC 掩码不对）\n",
                   tag, fname, (unsigned long long)raw, (unsigned long long)nxt);
            return 0;
        }
        if (nxt == cur) {
            printf("[krw] scan(%s) 停止：proc=0x%llx 的 %s 自指\n",
                   tag, (unsigned long long)cur, fname);
            return 0;
        }
        if (nxt == start) {
            printf("[krw] scan(%s) 走满一圈回到起点 0x%llx（共 %llu 个节点）\n",
                   tag, (unsigned long long)start, (unsigned long long)(step + 1ull));
            return 0;
        }
        cur = nxt;
    }

    printf("[krw] scan(%s)：%llu 步硬上限内没找到 pid=%d\n",
           tag, (unsigned long long)kMaxProcScan, pid);
    return 0;
}

inline uint64_t find_proc(const Ctx& c, int pid)
{
    if (c.kfd == 0) {
        printf("[krw] find_proc 失败：Ctx 没 init（kfd=0）\n");
        return 0;
    }

    // 起点必须选在 allproc 链表里的节点：优先 current_proc。
    // （kernel_proc 是 kernel_task 的 proc，实测它的 le_next=0，不在链上。）
    uint64_t start = c.current_proc ? c.current_proc : c.kernel_proc;
    if (start == 0) {
        printf("[krw] find_proc 失败：current_proc 与 kernel_proc 都是 0\n");
        return 0;
    }
    printf("[krw] find_proc 起点=0x%llx（%s），目标 pid=%d\n",
           (unsigned long long)start,
           c.current_proc ? "current_proc" : "kernel_proc（回退）",
           pid);

    uint64_t r = scan_dir(c, start, pid, true, "forward");
    if (r != 0) return r;

    printf("[krw] find_proc：正向没命中，改从起点反向扫（le_prev）\n");
    return scan_dir(c, start, pid, false, "backward");
}'''

src = src[:i] + new_fn + src[end:]
open(P, "w", encoding="utf-8", newline="\n").write(src)

# 括号自检
depth = 0
for ln in src.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
print("KrwProc.h depth =", depth)
print("has current_proc:", "current_proc" in src)
print("has scan_dir:", "inline uint64_t scan_dir" in src)
