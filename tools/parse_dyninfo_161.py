import re
import os

P = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\forks\Picasso-v3__Picasso_Exploit_KFDSwift_libkfd_info_dynamic_info.h"
txt = open(P, encoding="utf-8", errors="replace").read()
print("文件 %d 字节" % len(txt))

# 按 "// iOS" 注释 + kern_version 切块
blocks = re.split(r"\n\s*\{", txt)
hits = []
for b in blocks:
    m = re.search(r'kern_version\s*=\s*"([^"]+)"', b)
    if not m:
        continue
    ver = m.group(1)
    if "22.1.0" in ver:          # Darwin 22.1.0 = iOS 16.1.x
        hits.append((ver, b))

print("含 22.1.0 的条目: %d 条\n" % len(hits))

KEYS = [
    "perf_supported", "kread_kqueue_workloop_ctl_supported", "krkw_iosurface_supported",
    "kernelcache__static_base", "kernelcache__allproc", "kernelcache__kernproc",
    "kernelcache__gVirtBase", "kernelcache__gPhysBase", "kernelcache__gPhysSize",
    "kernelcache__ptov_table", "kernelcache__cpu_ttep", "kernelcache__vm_page_array",
    "kernelcache__kalloc_data_external", "kernelcache__kfree_data_external",
    "proc__object_size", "proc__p_pid", "proc__p_list__le_prev", "proc__p_fd__fd_ofiles",
    "task__map", "task__itk", "vm_map__pmap", "vm_map__object_size", "thread__thread_id",
]

for ver, b in hits:
    print("=" * 70)
    print(ver)
    for k in KEYS:
        m = re.search(re.escape(k) + r"\s*=\s*([^,\n]+)", b)
        if m:
            print("    %-40s = %s" % (k, m.group(1).strip()))
    # 打印这个块里全部 kernelcache__ 字段
    kc = re.findall(r"(kernelcache__\w+)\s*=\s*([^,\n]+)", b)
    if kc:
        print("    --- 全部 kernelcache__ 字段 (%d) ---" % len(kc))
        for name, val in kc:
            print("      %-46s = %s" % (name, val.strip()))
