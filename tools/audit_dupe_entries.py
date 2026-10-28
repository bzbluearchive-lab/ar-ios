import re

P = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\kfd161\libkfd\info\dynamic_info.h"
txt = open(P, encoding="utf-8", errors="replace").read()
KEY = "Thu Oct  6 19:34:00 PDT 2022; root:xnu-8792.42.7~1/RELEASE_ARM64_T8112"

# 按顶层条目切：( 结构体是 { ... }, 每项以 "{ ... }," 结束)
# 用 "kern_version" 定位每条，然后向前找最近的 "{"、向后找最近的 "},"
pos = []
start = 0
while True:
    i = txt.find(KEY, start)
    if i < 0:
        break
    pos.append(i)
    start = i + 1

print("同一 kern_version 出现 %d 次\n" % len(pos))

FIELDS = ["device_id", "os_version", "build_id",
          "kernelcache__kernel_base", "kernelcache__vn_kqfilter",
          "kernelcache__ptov_table", "kernelcache__gVirtBase",
          "proc__p_pid", "proc__p_list__le_prev", "proc__p_fd__fd_ofiles",
          "proc__object_size", "task__map", "task__itk_space", "task__object_size",
          "thread__thread_id", "thread__object_size", "pmap__tte", "pmap__ttep"]

def entry_of(idx):
    l = txt.rfind("{", 0, idx)
    r = txt.find("},", idx)
    return l, r, txt[l:r]

sig_groups = {}
for n, i in enumerate(pos, 1):
    l, r, blk = entry_of(i)
    vals = {}
    for f in FIELDS:
        # 跳过被注释掉的字段（行首有 //）
        m = re.search(r"^\s*\." + re.escape(f) + r"\s*=\s*([^,\n]+)", blk, re.M)
        vals[f] = m.group(1).strip() if m else "<缺失/注释>"
    sig = (vals["proc__p_pid"], vals["proc__object_size"], vals["task__itk_space"],
           vals["thread__thread_id"], vals["kernelcache__kernel_base"], vals["kernelcache__vn_kqfilter"])
    sig_groups.setdefault(sig, []).append((n, i, l, r, vals))

print("按关键偏移分组：%d 组\n" % len(sig_groups))
gi = 0
for sig, items in sig_groups.items():
    gi += 1
    print("=" * 72)
    print("组 %d：%d 个条目共享同一组偏移" % (gi, len(items)))
    print("  签名 proc__p_pid=%s proc__object_size=%s task__itk_space=%s thread__thread_id=%s" % sig[:4])
    print("       kernel_base=%s vn_kqfilter=%s" % sig[4:])
    for n, i, l, r, v in items:
        print("   ── 第 %d 次出现 @%d (条目 %d..%d)" % (n, i, l, r))
        print("       device=%-12s os=%-8s build=%-8s" % (v["device_id"], v["os_version"], v["build_id"]))
        print("       p_pid=%s p_list_le_prev=%s p_fd_ofiles=%s" % (v["proc__p_pid"], v["proc__p_list__le_prev"], v["proc__p_fd__fd_ofiles"]))
        print("       task__map=%s itk_space=%s task_size=%s thread_id=%s" % (v["task__map"], v["task__itk_space"], v["task__object_size"], v["thread__thread_id"]))
        print("       ptov=%s gVirtBase=%s pmap_tte=%s pmap_ttep=%s" % (v["kernelcache__ptov_table"], v["kernelcache__gVirtBase"], v["pmap__tte"], v["pmap__ttep"]))
