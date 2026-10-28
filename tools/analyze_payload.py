import os
import re
import struct
import zipfile

W = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\rsr_test"
TAR = os.path.join(W, "apple.tar")
APPL = os.path.join(W, "apple")

# ---------- apple.tar 其实是 zip ----------
print("=== apple.tar 条目 ===")
with zipfile.ZipFile(TAR) as z:
    for info in z.infolist()[:40]:
        print("  %-56s %10d B" % (info.filename, info.file_size))
    names = z.namelist()
    print("条目总数: %d" % len(names))

    # 找其中的 Mach-O / dylib / plist
    os.makedirs(os.path.join(W, "tar_x"), exist_ok=True)
    for n in names:
        low = n.lower()
        if low.endswith((".dylib", ".plist", ".json", ".bin")) or "payload" in low:
            dst = os.path.join(W, "tar_x", os.path.basename(n))
            with z.open(n) as f, open(dst, "wb") as o:
                o.write(f.read())
            print("  解出 %-40s %d B" % (os.path.basename(n), os.path.getsize(dst)))

# ---------- apple 这个 Mach-O ----------
print("\n=== apple (Mach-O) ===")
d = open(APPL, "rb").read()
print("大小 %d B  magic=%s" % (len(d), d[:4].hex()))
# 小端解析
if d[:4] == b"\xcf\xfa\xed\xfe" or d[:4] == b"\xfe\xed\xfa\xcf":
    magic = struct.unpack("<I", d[:4])[0]
    print("按小端读 magic=0x%08x" % magic)

KFD = [b"gVirtBase", b"ptov_table", b"cpu_ttep", b"physmap", b"landa", b"smith",
       b"physpuppet", b"kread", b"kwrite", b"GWorld", b"UWorld", b"GNames",
       b"FNamePool", b"bone", b"Bone", b"ESP", b"ImGui", b"esp", b"overlay"]
print("特征扫描:")
for k in KFD:
    c = len(re.findall(re.escape(k), d))
    if c:
        print("  %-14s %d 处" % (k.decode(), c))

# 段表，看它是什么类型的东西
if d[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
    try:
        cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack("<iiIIII", d[4:28])
        print("filetype=%d (6=dylib 8=bundle 2=exe) ncmds=%d" % (ftype, ncmds))
        p = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack("<II", d[p:p + 8])
            if cmd == 0x19:
                seg = d[p + 8:p + 24].rstrip(b"\x00").decode("ascii", "ignore")
                vmaddr, vmsize, fileoff, filesize = struct.unpack("<QQQQ", d[p + 24:p + 56])
                print("  seg %-16s vm=0x%x size=0x%x" % (seg, vmaddr, vmsize))
            p += cmdsize
    except Exception as e:
        print("  段表解析失败:", e)

print("\n完成")
