// MachoTool.cs - minimal Mach-O / ARM64 static helper for the UAGame analysis
//
// Built with the age-old csc.exe that ships with .NET Framework, so it must stay
// C# 5 compatible (no string interpolation, no expression-bodied members, no tuples).
//
// Commands:
//   adrp       <bin> <targetVa>                          list adrp+add sites that land exactly on targetVa
//   adrpdata   <bin> <vaStart> <vaEnd> <dataLo> <dataHi> inside a code range, list adrp->data[Lo,Hi) + following ldr/str
//   dis        <bin> <va> <count>                        decode count instructions from va
//   funcstart  <bin> <va>                                walk back to a plausible prologue
//
// All addresses are hex without 0x prefix accepted either way.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

static class MachoTool
{
    // ---------- Mach-O ----------
    sealed class Seg { public string Name; public ulong VmAddr, VmSize, FileOff, FileSize; }
    static byte[] Buf;
    static List<Seg> Segs = new List<Seg>();

    static uint U32(long o) { return (uint)(Buf[o] | (Buf[o + 1] << 8) | (Buf[o + 2] << 16) | (Buf[o + 3] << 24)); }
    static ulong U64(long o) { return (ulong)U32(o) | ((ulong)U32(o + 4) << 32); }

    static void Load(string path)
    {
        Buf = File.ReadAllBytes(path);
        uint magic = U32(0);
        if (magic != 0xFEEDFACFu) throw new Exception("not a thin little-endian 64-bit Mach-O");
        uint ncmds = U32(16);
        long pos = 32;
        for (uint i = 0; i < ncmds; i++)
        {
            uint cmd = U32(pos), sz = U32(pos + 4);
            if (sz == 0) break;
            if (cmd == 0x19)
            {
                var s = new Seg();
                char[] nm = new char[16];
                for (int k = 0; k < 16; k++) nm[k] = (char)Buf[pos + 8 + k];
                s.Name = new string(nm).TrimEnd('\0');
                s.VmAddr = U64(pos + 24); s.VmSize = U64(pos + 32);
                s.FileOff = U64(pos + 40); s.FileSize = U64(pos + 48);
                Segs.Add(s);
            }
            pos += sz;
        }
    }

    static Seg SegOf(string name)
    {
        foreach (var s in Segs) if (s.Name == name) return s;
        return null;
    }

    // VA -> file offset (only for backed bytes)
    static long FileOf(ulong va)
    {
        foreach (var s in Segs)
        {
            if (va >= s.VmAddr && va < s.VmAddr + s.FileSize) return (long)(s.FileOff + (va - s.VmAddr));
        }
        return -1;
    }

    // ---------- ARM64 ----------
    static bool IsAdrp(uint w) { return (w & 0x9F000000u) == 0x90000000u; }

    static ulong AdrpTarget(uint w, ulong pc)
    {
        long immlo = (long)((w >> 29) & 3);
        long immhi = (long)((w >> 5) & 0x7FFFF);
        long imm = (immhi << 2) | immlo;
        if ((imm & (1L << 20)) != 0) imm -= (1L << 21);
        ulong page = pc & ~0xFFFUL;
        return (ulong)((long)page + (imm << 12));
    }

    static bool IsAddImm64(uint w, out int rd, out int rn, out long imm)
    {
        rd = 0; rn = 0; imm = 0;
        if ((w & 0x7F800000u) != 0x11000000u) return false;
        if (((w >> 31) & 1) != 1) return false;
        rd = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        int imm12 = (int)((w >> 10) & 0xFFF);
        int sh = (int)((w >> 22) & 3);
        imm = (sh == 1) ? ((long)imm12 << 12) : imm12;
        return true;
    }

    static bool IsLdrUimm(uint w, out int rt, out int rn, out int off)
    {
        rt = 0; rn = 0; off = 0;
        if ((w & 0xFFC00000u) != 0xF9400000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        off = (int)((w >> 10) & 0xFFF) * 8;
        return true;
    }

    static bool IsStrUimm(uint w, out int rt, out int rn, out int off)
    {
        rt = 0; rn = 0; off = 0;
        if ((w & 0xFFC00000u) != 0xF9000000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        off = (int)((w >> 10) & 0xFFF) * 8;
        return true;
    }

    static string Decode(uint w, ulong pc)
    {
        if (IsAdrp(w)) return string.Format("adrp x{0}, 0x{1:x}", w & 0x1F, AdrpTarget(w, pc));
        int rd, rn, off; long imm;
        if (IsAddImm64(w, out rd, out rn, out imm)) return string.Format("add x{0}, x{1}, #0x{2:x}", rd, rn, imm);
        if (IsLdrUimm(w, out rd, out rn, out off)) return string.Format("ldr x{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsStrUimm(w, out rd, out rn, out off)) return string.Format("str x{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        // STR/LDR 32-bit（属性表里常见）
        if ((w & 0xFFC00000u) == 0xB9000000u)
        {
            int rt = (int)(w & 0x1F), rb = (int)((w >> 5) & 0x1F), o = (int)((w >> 10) & 0xFFF) * 4;
            return string.Format("str w{0}, [x{1}, #0x{2:x}]", rt, rb, o);
        }
        if ((w & 0xFFC00000u) == 0xB9400000u)
        {
            int rt = (int)(w & 0x1F), rb = (int)((w >> 5) & 0x1F), o = (int)((w >> 10) & 0xFFF) * 4;
            return string.Format("ldr w{0}, [x{1}, #0x{2:x}]", rt, rb, o);
        }
        // MOVZ / MOVN / MOVK
        if ((w & 0x7F800000u) == 0x52800000u || (w & 0x7F800000u) == 0xD2800000u)
        {
            bool is64 = ((w & 0x80000000u) != 0);
            int r = (int)(w & 0x1F);
            long v = (long)((w >> 5) & 0xFFFF) << (int)(((w >> 21) & 3) * 16);
            return string.Format("mov {0}{1}, #0x{2:x}", is64 ? "x" : "w", r, v);
        }
        if ((w & 0x7F800000u) == 0x12800000u || (w & 0x7F800000u) == 0x92800000u)
        {
            bool is64 = ((w & 0x80000000u) != 0);
            int r = (int)(w & 0x1F);
            long v = (long)((w >> 5) & 0xFFFF) << (int)(((w >> 21) & 3) * 16);
            v = ~v;
            return string.Format("mov {0}{1}, #0x{2:x}   (movn)", is64 ? "x" : "w", r, v);
        }
        if ((w & 0xFC000000u) == 0x94000000u)
        {
            long i26 = w & 0x3FFFFFF;
            if ((i26 & (1L << 25)) != 0) i26 -= (1L << 26);
            return string.Format("bl 0x{0:x}", (ulong)((long)pc + i26 * 4));
        }
        if (w == 0xD65F03C0u) return "ret";
        if (w == 0xD503201Fu) return "nop";
        if ((w & 0xFFC07C1Fu) == 0xA9807BFDu) return "stp x29, x30, [sp, #-...]!";
        if ((w & 0xFFE00C00u) == 0xB4000000u) return "cbz/cbnz";
        return string.Format(".word 0x{0:x8}", w);
    }

    static ulong H(string s)
    {
        if (s.StartsWith("0x") || s.StartsWith("0X")) s = s.Substring(2);
        return ulong.Parse(s, NumberStyles.HexNumber);
    }

    // ---------- commands ----------
    static void CmdAdrp(string bin, ulong targetVa)
    {
        Load(bin);
        var text = SegOf("__TEXT");
        long start = (long)text.FileOff;
        long end = (long)(text.FileOff + text.FileSize);
        ulong vaBase = text.VmAddr;
        int hits = 0;
        for (long i = start; i + 12 < end; i += 4)
        {
            uint w0 = U32(i);
            if (!IsAdrp(w0)) continue;
            ulong pc = vaBase + (ulong)(i - start);
            ulong page = targetVa & ~0xFFFUL;
            if (AdrpTarget(w0, pc) != page) continue;
            uint w1 = U32(i + 4);
            int rd, rn; long imm;
            if (!IsAddImm64(w1, out rd, out rn, out imm)) continue;
            if ((int)(w0 & 0x1F) != rn || rd != rn) continue;
            if ((ulong)((long)page + imm) != targetVa) continue;
            Console.WriteLine("  adrp+add  0x{0:x}   ({1})", pc, Decode(w0, pc) + " ; " + Decode(w1, pc + 4));
            hits++;
        }
        Console.WriteLine("hits={0} for 0x{1:x}", hits, targetVa);
    }

    static void CmdAdrpData(string bin, ulong vaStart, ulong vaEnd, ulong dataLo, ulong dataHi)
    {
        Load(bin);
        int hits = 0;
        for (ulong va = vaStart; va < vaEnd; va += 4)
        {
            long f = FileOf(va);
            if (f < 0) continue;
            uint w = U32(f);
            if (!IsAdrp(w)) continue;
            int adrpRd = (int)(w & 0x1F);
            ulong baseVal = AdrpTarget(w, va);
            // 关键：ADRP 之后编译器常用 add xN, xN, #imm 调整到具体全局，
            // 必须跟进去，否则会把页面基址误当成全局地址。
            ulong cur = baseVal;
            for (int k = 1; k <= 8; k++)
            {
                ulong i2 = va + (ulong)(4 * k);
                long f2 = FileOf(i2);
                if (f2 < 0) break;
                uint w2 = U32(f2);
                int rd, rn; long imm;
                if (IsAddImm64(w2, out rd, out rn, out imm) && rn == adrpRd && rd == adrpRd)
                {
                    cur = (ulong)((long)cur + imm);
                    continue;
                }
                int rt, rn2, off;
                bool isStr = IsStrUimm(w2, out rt, out rn2, out off);
                bool isLdr = IsLdrUimm(w2, out rt, out rn2, out off);
                if (!isStr && !isLdr) continue;
                if (rn2 != adrpRd) continue;
                ulong slot = (ulong)((long)cur + off);
                if (slot < dataLo || slot >= dataHi) continue;
                Console.WriteLine("  0x{0:x}  adrp x{1}, 0x{2:x} (+0x{3:x}) -> 0x{4:x}  {5} x{6}, [x{1}, #0x{7:x}]  => GLOBAL 0x{8:x}",
                    va, adrpRd, baseVal, cur - baseVal, i2, isStr ? "str" : "ldr", rt, off, slot);
                hits++;
                break;
            }
        }
        Console.WriteLine("globals-候选={0}", hits);
    }

    // 精确统计某个全局槽的访问点：ADRP(+ADD) 之后 ldr/str 恰好落到 addr
    static void CmdXref(string bin, ulong addr)
    {
        Load(bin);
        var text = SegOf("__TEXT");
        long start = (long)text.FileOff, end = (long)(text.FileOff + text.FileSize);
        ulong vaBase = text.VmAddr;
        int hits = 0;
        for (long i = start; i + 40 < end; i += 4)
        {
            uint w = U32(i);
            if (!IsAdrp(w)) continue;
            int adrpRd = (int)(w & 0x1F);
            ulong cur = AdrpTarget(w, vaBase + (ulong)(i - start));
            for (int k = 1; k <= 8; k++)
            {
                uint w2 = U32(i + 4 * k);
                int rd, rn; long imm;
                if (IsAddImm64(w2, out rd, out rn, out imm) && rn == adrpRd && rd == adrpRd)
                {
                    cur = (ulong)((long)cur + imm);
                    continue;
                }
                int rt, rn2, off;
                bool isStr = IsStrUimm(w2, out rt, out rn2, out off);
                bool isLdr = IsLdrUimm(w2, out rt, out rn2, out off);
                if (!isStr && !isLdr) continue;
                if (rn2 != adrpRd) continue;
                if ((ulong)((long)cur + off) != addr) break;
                Console.WriteLine("  0x{0:x}  {1} x{2}, [x{3}, #0x{4:x}]",
                    vaBase + (ulong)(i - start), isStr ? "str" : "ldr", rt, adrpRd, off);
                hits++;
                break;
            }
        }
        Console.WriteLine("xref-count 0x{0:x} = {1}", addr, hits);
    }

    static void CmdDis(string bin, ulong va, int count)
    {
        Load(bin);
        for (int i = 0; i < count; i++)
        {
            ulong a = va + (ulong)(4 * i);
            long f = FileOf(a);
            if (f < 0) { Console.WriteLine("0x{0:x}  <unmapped>", a); continue; }
            uint w = U32(f);
            Console.WriteLine("0x{0:x}  {1:x8}  {2}", a, w, Decode(w, a));
        }
    }

    static void CmdFuncStart(string bin, ulong va)
    {
        Load(bin);
        ulong limit = va - 0x6000;
        ulong found = va;
        for (ulong a = va & ~3UL; a >= limit; a -= 4)
        {
            long f = FileOf(a);
            if (f < 0) break;
            uint w = U32(f);
            if ((w & 0xFFC07C1Fu) == 0xA9807BFDu) { found = a; break; }
            long fp = FileOf(a - 4);
            if (fp >= 0 && U32(fp) == 0xD65F03C0u) { found = a; break; }
            if (a < 4) break;
        }
        Console.WriteLine("funcstart 0x{0:x} (searched back from 0x{1:x})", found, va);
    }

    // 字段偏移直方图：对某个全局（如 GEngine）的每个引用点，
    // 抓"载入该全局到 xR 之后紧跟的 ldr/str xM,[xR,#off]"的 off，按频次排序。
    // 高频 off 就是热字段（GameViewport / GameInstance / GameView 这类每次 tick 都碰的）。
    static void CmdFieldHist(string bin, ulong globalAddr, int topN)
    {
        Load(bin);
        var text = SegOf("__TEXT");
        long start = (long)text.FileOff, end = (long)(text.FileOff + text.FileSize);
        ulong vaBase = text.VmAddr;
        var hist = new Dictionary<int, int>();
        var sample = new Dictionary<int, ulong>();
        for (long i = start; i + 60 < end; i += 4)
        {
            uint w = U32(i);
            if (!IsAdrp(w)) continue;
            int adrpRd = (int)(w & 0x1F);
            ulong cur = AdrpTarget(w, vaBase + (ulong)(i - start));
            int loadReg = -1;
            int loadIdx = -1;
            for (int k = 1; k <= 8; k++)
            {
                uint w2 = U32(i + 4 * k);
                int rd, rn; long imm;
                if (IsAddImm64(w2, out rd, out rn, out imm) && rn == adrpRd && rd == adrpRd)
                {
                    cur = (ulong)((long)cur + imm);
                    continue;
                }
                int rt, rn2, off;
                bool isStr = IsStrUimm(w2, out rt, out rn2, out off);
                bool isLdr = IsLdrUimm(w2, out rt, out rn2, out off);
                if (!isStr && !isLdr) continue;
                if (rn2 != adrpRd) continue;
                if ((ulong)((long)cur + off) == globalAddr)
                {
                    loadReg = rt;
                    loadIdx = k;
                }
                break;
            }
            if (loadReg < 0) continue;

            // 往后 12 条指令里找以 loadReg 为基址的 ldr/str。
            // 关键：遇到 bl/blr 时 x0-x18 会被覆盖，必须让跟踪失效；
            // 只有 x19-x28 是 callee-saved，可以跨调用继续跟踪。
            for (int k = loadIdx + 1; k <= loadIdx + 12; k++)
            {
                uint w2 = U32(i + 4 * k);
                bool isCall = ((w2 & 0xFC000000u) == 0x94000000u) || ((w2 & 0xFFFFFC1Fu) == 0xD63F0000u);
                if (isCall && loadReg < 19) break;      // caller-saved 被破坏
                int rt, rn2, off;
                bool isStr = IsStrUimm(w2, out rt, out rn2, out off);
                bool isLdr = IsLdrUimm(w2, out rt, out rn2, out off);
                if (!isStr && !isLdr) continue;
                if (rn2 != loadReg) continue;
                if (!hist.ContainsKey(off)) { hist[off] = 0; sample[off] = vaBase + (ulong)(i - start); }
                hist[off] = hist[off] + 1;
                break;
            }
        }

        var list = new List<KeyValuePair<int, int>>(hist);
        list.Sort(delegate (KeyValuePair<int, int> a, KeyValuePair<int, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("field-offset histogram for global 0x{0:x}  ({1} distinct offsets)", globalAddr, list.Count);
        int n = 0;
        foreach (var kv in list)
        {
            Console.WriteLine("  +0x{0,-6:x} x{1,-5}  first-site 0x{2:x}", kv.Key, kv.Value, sample[kv.Key]);
            if (++n >= topN) break;
        }
    }

    // 找全文件里指向某地址的 8 字节指针（属性注册表里 NameUTF8 那种），
    // 并按 UE4 FPropertyParams 的猜测布局解析上下文：
    //   +0x00 NameUTF8(ptr)  +0x08 RepNotify(ptr)  +0x10 PropFlags(u64)  +0x18 ArrayDim(u32)  +0x1C Offset(u32)
    static void CmdPtrRef(string bin, ulong target, int maxHits)
    {
        Load(bin);
        var text = SegOf("__TEXT"); var data = SegOf("__DATA");
        ulong baseAll = text.VmAddr;
        int hits = 0;
        for (long i = 0; i + 64 < Buf.Length; i += 8)
        {
            if (U64(i) != target) continue;
            ulong va = VaOf((ulong)i);
            if (va == 0) continue;
            ulong f8 = U64(i + 8);
            ulong f16 = U64(i + 16);
            uint a24 = U32(i + 24);
            uint a28 = U32(i + 28);
            Console.WriteLine("  ptr@VA 0x{0:x}   [name ptr]  +8=0x{1:x}  +16(flags)=0x{2:x}  +24(ArrayDim)=0x{3:x}  +28(Offset)=0x{4:x}",
                va, f8, f16, a24, a28);
            hits++;
            if (hits >= maxHits) break;
        }
        Console.WriteLine("ptrref hits = {0} (target 0x{1:x})", hits, target);
    }

    // file offset -> VA
    static ulong VaOf(ulong fileOff)
    {
        foreach (var s in Segs)
        {
            if (s.FileSize == 0) continue;
            if (fileOff >= s.FileOff && fileOff < s.FileOff + s.FileSize)
                return s.VmAddr + (fileOff - s.FileOff);
        }
        return 0;
    }

    // 反汇编并把 adrp(+add) 指向 __cstring 的引用解析成实际字符串，
    // 用来判断某段代码在操作什么（UE 的日志串常常直接点名当前动作）。
    static void CmdDisX(string bin, ulong va, int count)
    {
        Load(bin);
        var cs = SegOf("__TEXT");
        for (int i = 0; i < count; i++)
        {
            ulong a = va + (ulong)(4 * i);
            long f = FileOf(a);
            if (f < 0) { Console.WriteLine("0x{0:x}  <unmapped>", a); continue; }
            uint w = U32(f);
            string line = string.Format("0x{0:x}  {1:x8}  {2}", a, w, Decode(w, a));
            if (IsAdrp(w))
            {
                int rd = (int)(w & 0x1F);
                ulong t = AdrpTarget(w, a);
                // 看下一条是否 add xN, xN, #imm
                long f2 = FileOf(a + 4);
                ulong addr = t;
                if (f2 >= 0)
                {
                    int rd2, rn2; long imm;
                    if (IsAddImm64(U32(f2), out rd2, out rn2, out imm) && rn2 == rd && rd2 == rd)
                        addr = (ulong)((long)t + imm);
                }
                string s = ReadCString(addr);
                if (s != null)
                {
                    if (s.Length > 64) s = s.Substring(0, 64) + "...";
                    line += "        ; \"" + s + "\"";
                }
            }
            Console.WriteLine(line);
        }
    }

    static string ReadCString(ulong va)
    {
        long f = FileOf(va);
        if (f < 0) return null;
        var sb = new StringBuilder();
        for (long i = f; i < Buf.Length && sb.Length < 200; i++)
        {
            byte b = Buf[i];
            if (b == 0) break;
            if (b < 0x20 || b > 0x7E) return null;   // 不是可打印 ASCII
            sb.Append((char)b);
        }
        return sb.Length >= 3 ? sb.ToString() : null;
    }

    // TArray 读取特征扫描：UE4 的 TArray = { Data*, Num(int32), Max(int32) }。
    // 迭代代码里必然出现同一基址寄存器上的成对读取：
    //     ldr xN, [xM, #O]        ; Data
    //     ldr wK, [xM, #O+8]      ; Num
    // 把这种配对按 O 统计频次，高频 O 就是热点数组（Actors 每帧都遍历）。
    static void CmdTArray(string bin, int topN)
    {
        Load(bin);
        var text = SegOf("__TEXT");
        long start = (long)text.FileOff, end = (long)(text.FileOff + text.FileSize);
        ulong vaBase = text.VmAddr;
        var hist = new Dictionary<int, int>();
        var sample = new Dictionary<int, ulong>();

        for (long i = start; i + 32 < end; i += 4)
        {
            uint w0 = U32(i);
            int rt, rn, off;
            if (!IsLdrUimm(w0, out rt, out rn, out off)) continue;   // ldr x, [xm, #O]
            int baseReg = rn;
            int o0 = off;
            for (int k = 1; k <= 6; k++)
            {
                uint w2 = U32(i + 4 * k);
                if ((w2 & 0xFFC00000u) == 0xB9400000u)               // ldr w, [xm, #O+8]
                {
                    int b2 = (int)((w2 >> 5) & 0x1F);
                    int o2 = (int)((w2 >> 10) & 0xFFF) * 4;
                    if (b2 == baseReg && o2 == o0 + 8)
                    {
                        if (!hist.ContainsKey(o0)) { hist[o0] = 0; sample[o0] = vaBase + (ulong)(i - start); }
                        hist[o0] = hist[o0] + 1;
                    }
                    break;
                }
            }
        }
        var list = new List<KeyValuePair<int, int>>(hist);
        list.Sort(delegate (KeyValuePair<int, int> a, KeyValuePair<int, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("TArray[Data,Num] 配对偏移直方图（distinct={0}）", list.Count);
        int n = 0;
        foreach (var kv in list)
        {
            Console.WriteLine("  O=0x{0,-5:x} x{1,-5} first-site 0x{2:x}", kv.Key, kv.Value, sample[kv.Key]);
            if (++n >= topN) break;
        }
    }

    // 找谁调用了某个函数（扫 BL 指令，按 pc + imm26*4 计算目标）
    static void CmdCallers(string bin, ulong target, int maxHits)
    {
        Load(bin);
        var text = SegOf("__TEXT");
        long start = (long)text.FileOff, end = (long)(text.FileOff + text.FileSize);
        ulong vaBase = text.VmAddr;
        int hits = 0;
        for (long i = start; i + 4 < end; i += 4)
        {
            uint w = U32(i);
            if ((w & 0xFC000000u) != 0x94000000u) continue;
            ulong pc = vaBase + (ulong)(i - start);
            long i26 = w & 0x3FFFFFF;
            if ((i26 & (1L << 25)) != 0) i26 -= (1L << 26);
            ulong t = (ulong)((long)pc + i26 * 4);
            if (t != target) continue;
            Console.WriteLine("  caller 0x{0:x}", pc);
            if (++hits >= maxHits) break;
        }
        Console.WriteLine("callers of 0x{0:x} = {1}", target, hits);
    }

    // 扫 FName 解析特征，定位 GNames 全局 与 Blocks[] 偏移：
    //   adrp xT, page ; ldr xT, [xT, #off]  ... lsr w?, w?, #16 ... add xT, xT, #imm2
    static void CmdFnPool(string bin, int topN)
    {
        Load(bin);
        var text = SegOf("__TEXT");
        long start = (long)text.FileOff, end = (long)(text.FileOff + text.FileSize);
        ulong vaBase = text.VmAddr;
        var hist = new Dictionary<string, int>();
        var sample = new Dictionary<string, ulong>();

        for (long i = start; i + 64 < end; i += 4)
        {
            uint w = U32(i);
            // LSR Wd, Wn, #16  -> 0x53107C00 | Rn<<5 | Rd
            if ((w & 0xFFC0FC00u) != 0x53107C00u) continue;
            ulong pc = vaBase + (ulong)(i - start);

            // 往回找最多 8 条：ldr xT,[xT,#imm] 且前一条是 adrp xT
            int ldrReg = -1; int ldrOff = 0; ulong adrpPage = 0;
            for (int k = 1; k <= 8; k++)
            {
                uint wl = U32(i - 4 * k);
                int rt, rn, off;
                if (!IsLdrUimm(wl, out rt, out rn, out off)) continue;
                if (rt != rn) continue;
                uint wa = U32(i - 4 * (k + 1));
                if (!IsAdrp(wa)) continue;
                if ((int)(wa & 0x1F) != rt) continue;
                ldrReg = rt; ldrOff = off;
                adrpPage = AdrpTarget(wa, pc - (ulong)(4 * (k + 1)));
                break;
            }
            if (ldrReg < 0) continue;
            ulong gnames = adrpPage + (ulong)ldrOff;

            // 往后找最多 6 条：add xT, xT, #imm2  -> Blocks 偏移
            int blocksOff = -1;
            for (int k = 1; k <= 6; k++)
            {
                uint w2 = U32(i + 4 * k);
                int rd, rn; long imm;
                if (IsAddImm64(w2, out rd, out rn, out imm) && rd == ldrReg && rn == ldrReg && imm > 0 && imm < 0x1000)
                { blocksOff = (int)imm; break; }
            }
            string key = string.Format("GNames=0x{0:x} blocksOff=0x{1:x}", gnames, blocksOff);
            if (!hist.ContainsKey(key)) { hist[key] = 0; sample[key] = pc; }
            hist[key] = hist[key] + 1;
        }

        var list = new List<KeyValuePair<string, int>>(hist);
        list.Sort(delegate (KeyValuePair<string, int> a, KeyValuePair<string, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("FName 特征命中组合 = {0}", list.Count);
        int n = 0;
        foreach (var kv in list)
        {
            Console.WriteLine("  {0}  x{1}  first-pc=0x{2:x}", kv.Key, kv.Value, sample[kv.Key]);
            if (++n >= topN) break;
        }
    }

    static int Main(string[] args)
    {
        try
        {
            if (args.Length < 3) { Console.WriteLine("usage: MachoTool <cmd> <bin> ..."); return 2; }
            string cmd = args[0], bin = args[1];
            switch (cmd)
            {
                case "adrp": CmdAdrp(bin, H(args[2])); break;
                case "adrpdata": CmdAdrpData(bin, H(args[2]), H(args[3]), H(args[4]), H(args[5])); break;
                case "xref": CmdXref(bin, H(args[2])); break;
                case "fieldhist": CmdFieldHist(bin, H(args[2]), int.Parse(args[3])); break;
                case "ptrref": CmdPtrRef(bin, H(args[2]), int.Parse(args[3])); break;
                case "disx": CmdDisX(bin, H(args[2]), int.Parse(args[3])); break;
                case "tarray": CmdTArray(bin, int.Parse(args[2])); break;
                case "callers": CmdCallers(bin, H(args[2]), int.Parse(args[3])); break;
                case "fnpool": CmdFnPool(bin, int.Parse(args[2])); break;
                case "dis": CmdDis(bin, H(args[2]), int.Parse(args[3])); break;
                case "funcstart": CmdFuncStart(bin, H(args[2])); break;
                default: Console.WriteLine("unknown cmd: " + cmd); return 2;
            }
            return 0;
        }
        catch (Exception e)
        {
            Console.WriteLine("ERROR: " + e.Message);
            return 1;
        }
    }
}
