import os
import re
import struct

BIN = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\rsr_test\播客.RS.R"
data = open(BIN, "rb").read()
print("二进制 %d 字节 (%.1f MB)\n" % (len(data), len(data) / 1048576.0))

# ---------- 1) 找内嵌的内核地址常量（0xfffffff0_xxxxxxxx 形态）----------
print("=== ① 内核地址形态常量 (0xfffffff0xxxxxxxx) ===")
pat = re.compile(b"\xf0\xff\xff\xff[\x00-\xff][\x00-\xff][\x00-\xff][\x00-\xff]|\xf0\xff\xff\xff\xff\xff\xff")
hits = []
for m in re.finditer(b"\xf0\xff\xff\xff", data):
    i = m.start()
    if i + 8 <= len(data):
        v = struct.unpack("<Q", data[i:i + 8])[0]
        if 0xFFFFFFF000000000 <= v <= 0xFFFFFFFFFFFFFFFF:
            hits.append((i, v))
print("  命中 %d 处" % len(hits))
for off, v in hits[:25]:
    print("    0x%08x -> 0x%016x" % (off, v))

# ---------- 2) kernelcache 符号名字符串在二进制里的上下文 ----------
print("\n=== ② kernelcache__* 符号字符串 ===")
for m in re.finditer(b"kernelcache__", data):
    i = m.start()
    end = data.find(b"\x00", i)
    name = data[i:end].decode("ascii", "replace")
    # 名字后面紧邻 8 字节是否是内核地址
    tail = struct.unpack("<Q", data[end + 1:end + 9])[0] if end + 9 <= len(data) else 0
    print("  0x%08x  %-40s 后随8字节=0x%016x" % (i, name, tail))

# ---------- 3) libgrabkernel 相关（说明是下载还是内嵌）----------
print("\n=== ③ libgrabkernel 相关字符串 ===")
seen = set()
for m in re.finditer(rb"[ -~]{6,}", data):
    s = m.group().decode("ascii", "replace")
    if "grabkernel" in s.lower() or "kernelcache" in s.lower():
        if s not in seen:
            seen.add(s)
            print("  0x%08x  %s" % (m.start(), s[:100]))

# ---------- 4) 是否有 proc/task 结构偏移的小整数表 ----------
print("\n=== ④ 可疑的 proc/task 偏移字段名字符串 ===")
for key in [b"proc__object_size", b"proc__p_pid", b"proc__p_list", b"task__map",
            b"proc__p_fd", b"task__itk", b"proc__p_csflags", b"proc__p_textvp"]:
    c = len(re.findall(re.escape(key), data))
    print("  %-22s %d 处" % (key.decode(), c))
