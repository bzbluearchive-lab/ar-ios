#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ue4_find_globals.py —— 在 UE4 手游的 Mach-O 里定位全局变量（GWorld / GNames / GObjects）

原理（不依赖 IDA）：
  UE4 二进制里通常保留了调试字符串 "GWorld"、"GNames"、"GObjects"。
  这些字符串被引擎的反射/初始化代码引用，引用形态是：
      ADRP Xn, page            ; 取字符串所在页
      ADD  Xn, Xn, #off        ; 加上页内偏移 -> 字符串地址
  而全局变量指针本身的操作形态是：
      ADRP Xm, page
      LDR  Xm, [Xm, #off]      ; 取出全局指针（这里就是 GWorld）

  所以：找到字符串 -> 找引用它的 ADRP+ADD -> 在附近找 ADRP+LDR -> 得到候选全局地址。
  纯 Python，只解码这两类指令，不引入 capstone。

用法：
  python ue4_find_globals.py UAGame            # 直接给二进制
  python ue4_find_globals.py UAGame --all      # 打印全部候选，不做过滤
输出：
  每个候选：符号名、全局地址(vmaddr)、所在指令地址、指令字节，便于进 IDA/Frida 复核。
"""

import struct
import sys
import os
import re

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2

TARGETS = ["GWorld", "GNames", "GObjects", "GEngine", "FNamePool", "UWorld", "PersistentLevel"]


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def slices(data):
    """返回 [(offset, size, cputype)]；非 fat 返回单元素"""
    magic_be = struct.unpack(">I", data[:4])[0]
    if magic_be in (FAT_MAGIC, FAT_MAGIC_64):
        nfat = struct.unpack(">I", data[4:8])[0]
        out = []
        base = 8
        for i in range(nfat):
            if magic_be == FAT_MAGIC:
                cputype, cpusub, off, size, align = struct.unpack(">iiIII", data[base:base + 20])
                base += 20
            else:
                cputype, cpusub, off, size, align, _ = struct.unpack(">iiQQII", data[base:base + 32])
                base += 32
            out.append((off, size, cputype))
        return out
    return [(0, len(data), 0)]


class MachO:
    def __init__(self, data, off, size):
        self.data = data[off:off + size]
        self.base_off = off
        self.segments = []      # (name, vmaddr, vmsize, fileoff, filesize)
        self.symtab = None
        self._parse()

    def _parse(self):
        if len(self.data) < 32:
            raise ValueError("too small")
        magic = struct.unpack("<I", self.data[:4])[0]
        if magic != MH_MAGIC_64:
            raise ValueError("not arm64 Mach-O (magic=0x%08x)" % magic)
        cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack(
            "<iiIIIII", self.data[4:32])
        self.filetype = filetype
        p = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack("<II", self.data[p:p + 8])
            if cmd == LC_SEGMENT_64:
                segname = self.data[p + 8:p + 24].rstrip(b"\x00").decode("ascii", "ignore")
                vmaddr, vmsize, fileoff, filesize = struct.unpack("<QQQQ", self.data[p + 24:p + 56])
                self.segments.append((segname, vmaddr, vmsize, fileoff, filesize))
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack("<IIII", self.data[p + 8:p + 24])
                self.symtab = (symoff, nsyms, stroff, strsize)
            p += cmdsize

    def vm_to_off(self, vmaddr):
        for name, vma, vmsize, foff, fsize in self.segments:
            if vma <= vmaddr < vma + vmsize and fsize:
                delta = vmaddr - vma
                if delta < fsize:
                    return foff + delta
        return None

    def off_to_vm(self, off):
        for name, vma, vmsize, foff, fsize in self.segments:
            if foff <= off < foff + fsize:
                return vma + (off - foff)
        return None

    def text(self):
        for name, vma, vmsize, foff, fsize in self.segments:
            if name == "__TEXT" and fsize:
                return vma, foff, fsize
        return None


def find_cstring(mo, s):
    """在文件里找以 NUL 结尾的精确字符串，返回 [(vmaddr)] """
    needle = s.encode() + b"\x00"
    out = []
    start = 0
    while True:
        i = mo.data.find(needle, start)
        if i < 0:
            break
        # 要求前面不是可见字符（避免匹配到更长的字符串中间）
        if i == 0 or not (32 <= mo.data[i - 1] < 127):
            vm = mo.off_to_vm(i)
            if vm:
                out.append(vm)
        start = i + 1
    return out


def adrp_target(pc, insn):
    immlo = (insn >> 29) & 0x3
    immhi = (insn >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= (1 << 21)
    return (pc & ~0xFFF) + (imm << 12)


def is_adrp(insn):
    return (insn & 0x9F000000) == 0x90000000


def adrp_rd(insn):
    return insn & 0x1F


def add_imm(insn):
    """ADD Xd, Xn, #imm  -> (rd, rn, imm) 或 None"""
    if (insn & 0x7F000000) != 0x11000000:
        return None
    rd = insn & 0x1F
    rn = (insn >> 5) & 0x1F
    imm12 = (insn >> 10) & 0xFFF
    shift = (insn >> 22) & 0x3
    if shift != 0:
        return None
    return rd, rn, imm12


def ldr_uoff(insn):
    """LDR Xt, [Xn, #imm] (64-bit unsigned offset) -> (rt, rn, off) 或 None"""
    if (insn & 0xFFC00000) != 0xF9400000:
        return None
    rt = insn & 0x1F
    rn = (insn >> 5) & 0x1F
    imm12 = (insn >> 10) & 0xFFF
    return rt, rn, imm12 * 8


def analyze(path, verbose=False):
    data = read_file(path)
    print("文件: %s (%d 字节)" % (os.path.basename(path), len(data)))

    results = {}
    for off, size, cputype in slices(data):
        try:
            mo = MachO(data, off, size)
        except ValueError as e:
            print("  跳过 slice: %s" % e)
            continue

        t = mo.text()
        if not t:
            print("  没有 __TEXT，跳过")
            continue
        text_vm, text_off, text_size = t
        print("  __TEXT vmaddr=0x%x fileoff=0x%x size=0x%x" % (text_vm, text_off, text_size))

        # 把 __TEXT 读成指令数组（每条 4 字节）
        n_insn = text_size // 4
        code = struct.unpack("<%dI" % n_insn, mo.data[text_off:text_off + n_insn * 4])

        for name in TARGETS:
            strs = find_cstring(mo, name)
            if not strs:
                continue
            print("  字符串 %-14s 出现 %d 次: %s" %
                  (name, len(strs), ", ".join("0x%x" % x for x in strs[:4])))

            for svm in strs:
                # 找引用该字符串的 ADRP+ADD
                for i in range(n_insn - 1):
                    insn0, insn1 = code[i], code[i + 1]
                    if not is_adrp(insn0):
                        continue
                    pc = text_vm + i * 4
                    page = adrp_target(pc, insn0)
                    rd0 = adrp_rd(insn0)
                    a = add_imm(insn1)
                    if not a or a[1] != rd0:
                        continue
                    rd, rn, imm = a
                    if page + imm != svm:
                        continue

                    # 命中：附近找 ADRP + LDR（全局指针）
                    lo = max(0, i - 64)
                    hi = min(n_insn - 2, i + 64)
                    for j in range(lo, hi):
                        j0, j1 = code[j], code[j + 1]
                        if not is_adrp(j0):
                            continue
                        pc2 = text_vm + j * 4
                        page2 = adrp_target(pc2, j0)
                        rd2 = adrp_rd(j0)
                        l = ldr_uoff(j1)
                        if not l or l[1] != rd2:
                            continue
                        rt, rn2, off2 = l
                        gvm = page2 + off2
                        key = (name, gvm)
                        results.setdefault(key, {
                            "name": name,
                            "global_vm": gvm,
                            "xref_insn": pc,
                            "ldr_insn": pc2,
                            "str_vm": svm,
                        })

    print("")
    if not results:
        print("没有找到候选。可能：二进制被剥符号且字符串被加密，或该引擎版本不含这些名字。")
        return 1

    print("候选全局变量（按可信度排序，需进 IDA/Frida 复核）：")
    for (name, gvm), v in sorted(results.items(), key=lambda kv: kv[1]["name"]):
        print("  %-12s 全局地址 vmaddr=0x%08x   引用指令=0x%08x   LDR=0x%08x   字符串=0x%08x"
              % (name, gvm, v["xref_insn"], v["ldr_insn"], v["str_vm"]))
    print("")
    print("下一步：")
    print("  1) 把 __TEXT 基址与这些 vmaddr 一起记下；运行时基址 = 模块加载基址 + (vmaddr - __TEXT.vmaddr)")
    print("  2) 用 Frida 在游戏进程里读该地址，确认指向的是有效的 UWorld（前 8 字节是指针且可读）")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(analyze(sys.argv[1], "--all" in sys.argv))
