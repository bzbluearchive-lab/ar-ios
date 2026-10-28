import os
import re
import json
import zipfile
import struct

SRC = r"C:\Users\92072\Downloads\播客.RS.R.客户端启动修复-测试版(1).tipa"
OUT = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\rsr_test"
BIN_NAME = "播客.RS.R"

os.makedirs(OUT, exist_ok=True)

# ---------- 1) 解包（只取需要的条目）----------
targets = ["Info.plist", BIN_NAME, "apple", "apple.tar"]
with zipfile.ZipFile(SRC) as z:
    for n in z.namelist():
        base = os.path.basename(n)
        if base in targets:
            dst = os.path.join(OUT, base)
            with z.open(n) as f, open(dst, "wb") as o:
                o.write(f.read())
            print("解出 %-12s %d 字节" % (base, os.path.getsize(dst)))

binpath = os.path.join(OUT, BIN_NAME)
data = open(binpath, "rb").read()
print("\n主二进制 %d 字节 (%.1f MB)" % (len(data), len(data) / 1048576.0))

# ---------- 2) Mach-O 基本信息 ----------
magic = struct.unpack("<I", data[:4])[0]
print("magic = 0x%08x (%s)" % (magic, "arm64 Mach-O" if magic == 0xFEEDFACF else "其它"))
if magic == 0xFEEDFACF:
    cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack("<iiIIII", data[4:28])
    print("filetype=%d ncmds=%d flags=0x%08x" % (ftype, ncmds, flags))

# ---------- 3) kfd / 内核读写关键特征 ----------
print("\n=== 内核读写特征（kfd 结构体字段名）===")
KFD = [
    b"gVirtBase", b"gPhysBase", b"gPhysSize", b"ptov_table", b"allproc",
    b"kalloc_data_external", b"kfree_data_external", b"cpu_ttep",
    b"vm_page_array", b"physmap", b"kernel_el", b"pointer_mask",
    b"T1SZ_BOOT", b"ARM_TT_L1_INDEX_MASK", b"kernelSymbol", b"kernelConstant",
    b"landa", b"smith", b"physpuppet", b"kread", b"kwrite", b"kfd",
    b"libgrabkernel", b"grabkernel", b"IOKit", b"IOSurfaceRoot",
    b"processor_set_tasks", b"task_get_special_port", b"pid_for_task",
    b"vm_read_overwrite", b"task_for_pid",
]
for k in KFD:
    hits = [m.start() for m in re.finditer(re.escape(k), data)]
    if hits:
        print("  %-26s %4d 处  首现 0x%x" % (k.decode(), len(hits), hits[0]))

# ---------- 4) 游戏/引擎相关 ----------
print("\n=== 引擎/目标游戏特征 ===")
ENG = [
    b"GWorld", b"UWorld", b"GNames", b"GObjects", b"FNamePool", b"PersistentLevel",
    b"BoneMatrix", b"ComponentToWorld", b"ViewMatrix", b"WorldToScreen",
    b"com.proximabeta", b"com.tencent", b"DeltaForce", b"UAGame", b"Arena",
    b"anogs", b"ACE", b"xigncode", b"TenProtect",
]
for k in ENG:
    hits = [m.start() for m in re.finditer(re.escape(k), data)]
    if hits:
        print("  %-22s %4d 处" % (k.decode(), len(hits)))

# ---------- 5) entitlements（CSSLOT 5，大端解析）----------
print("\n=== entitlements ===")


def find_entitlements(d):
    """在 __LINKEDIT 里找 CodeDirectory 与 entitlements blob"""
    # 暴力找 SuperBlob magic 0xfade0cc0（大端）
    res = []
    i = 0
    while True:
        i = d.find(b"\xfa\xde\x0c\xc0", i)
        if i < 0:
            break
        try:
            magic, length, count = struct.unpack(">III", d[i:i + 12])
            off = i + 12
            for _ in range(count):
                slot_type, slot_off = struct.unpack(">II", d[off:off + 8])
                off += 8
                if slot_type == 5:  # entitlements
                    b_off = i + slot_off
                    b_magic, b_len = struct.unpack(">II", d[b_off:b_off + 8])
                    blob = d[b_off + 8:b_off + b_len]
                    res.append(blob)
        except Exception:
            pass
        i += 4
    return res


ents = find_entitlements(data)
if not ents:
    print("  没找到（可能被签名剥离或不在主二进制）")
else:
    for b in ents[:2]:
        txt = b.decode("utf-8", "replace")
        for line in txt.split("\n"):
            if line.strip():
                print("  " + line.strip())

# ---------- 6) 内嵌 payload（apple / apple.tar）----------
print("\n=== 内嵌载荷 apple / apple.tar ===")
for name in ("apple", "apple.tar"):
    p = os.path.join(OUT, name)
    if not os.path.exists(p):
        continue
    d = open(p, "rb").read()
    print("  %-10s %d 字节  magic=%s" % (name, len(d), d[:4].hex()))
    # 递归找 kfd 特征
    for k in (b"gVirtBase", b"ptov_table", b"cpu_ttep", b"landa", b"physpuppet", b"GWorld", b"UWorld"):
        c = len(re.findall(re.escape(k), d))
        if c:
            print("      含 %-14s %d 处" % (k.decode(), c))

print("\n完成。工作目录: %s" % OUT)
