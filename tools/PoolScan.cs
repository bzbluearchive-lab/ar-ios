// PoolScan.cs - AArch64 / Mach-O static scanner aimed at UE4 FNamePool (GNames).
// ASCII only on purpose: the previous version had CJK comments that got mangled
// by a PowerShell Get-Content/Set-Content round-trip.
// C#5 compatible (csc.exe from .NET Framework).
//
// Commands
//   shift    <bin> [minCount]                histogram of UBFM/LSR/AND-imm forms
//   resolve  <bin> [maxPrint]                ldr/str [constBase, idx<<k] sites (Blocks[..] shapes)
//   globals  <bin> <offCsv|-> <maxPrint>     adrp-const -> ldr/str [G, #off]
//   fn16     <bin> <shift> <maxWin>          windows around  lsr w?,w?,#shift
//   allocblk <bin> <imm> <half> <maxWin>     windows around  mov w?, #imm   (block malloc)
//   ctx      <bin> <va> <count>              disassemble

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

static class PoolScan
{
    sealed class Seg { public string Name; public ulong VmAddr, VmSize, FileOff, FileSize; }
    static byte[] Buf;
    static List<Seg> Segs = new List<Seg>();

    const ulong TEXT_LO = 0x100004000UL;
    const ulong TEXT_SIZE = 0xA41FFA8UL;
    const ulong DATA_LO = 0x10B9C8000UL;
    const ulong DATA_HI = 0x10DACAF70UL;

    static uint U32(long o) { return (uint)(Buf[o] | (Buf[o + 1] << 8) | (Buf[o + 2] << 16) | (Buf[o + 3] << 24)); }
    static ulong U64(long o) { return (ulong)U32(o) | ((ulong)U32(o + 4) << 32); }

    static void Load(string path)
    {
        Buf = File.ReadAllBytes(path);
        if (U32(0) != 0xFEEDFACFu) throw new Exception("not a thin little-endian 64-bit Mach-O");
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

    static long FileOf(ulong va)
    {
        foreach (var s in Segs)
            if (va >= s.VmAddr && va < s.VmAddr + s.FileSize) return (long)(s.FileOff + (va - s.VmAddr));
        return -1;
    }

    static string ReadCString(ulong va)
    {
        long f = FileOf(va);
        if (f < 0) return null;
        var sb = new StringBuilder();
        for (long i = f; i < Buf.Length && sb.Length < 120; i++)
        {
            byte b = Buf[i];
            if (b == 0) break;
            if (b < 0x20 || b > 0x7E) return null;
            sb.Append((char)b);
        }
        return sb.Length >= 3 ? sb.ToString() : null;
    }

    static ulong H(string s)
    {
        if (s.StartsWith("0x") || s.StartsWith("0X")) s = s.Substring(2);
        return ulong.Parse(s, NumberStyles.HexNumber);
    }

    // ---------------- decoders ----------------
    static bool IsAdrp(uint w) { return (w & 0x9F000000u) == 0x90000000u; }
    static ulong AdrpTarget(uint w, ulong pc)
    {
        long immlo = (long)((w >> 29) & 3);
        long immhi = (long)((w >> 5) & 0x7FFFF);
        long imm = (immhi << 2) | immlo;
        if ((imm & (1L << 20)) != 0) imm -= (1L << 21);
        return (ulong)((long)(pc & ~0xFFFUL) + (imm << 12));
    }
    static bool IsAddImm64(uint w, out int rd, out int rn, out long imm, out bool shift12)
    {
        rd = rn = 0; imm = 0; shift12 = false;
        if ((w & 0x7F000000u) != 0x11000000u) return false;
        int sh = (int)((w >> 22) & 3);
        if (sh == 2 || sh == 3) return false;
        rd = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        long imm12 = (long)((w >> 10) & 0xFFF);
        shift12 = (sh == 1);
        imm = shift12 ? (imm12 << 12) : imm12;
        return true;
    }
    static bool IsLdr64(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        if ((w & 0xFFC00000u) != 0xF9400000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF) * 8;
        return true;
    }
    static bool IsStr64(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        if ((w & 0xFFC00000u) != 0xF9000000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF) * 8;
        return true;
    }
    static bool IsLdr32(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        if ((w & 0xFFC00000u) != 0xB9400000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF) * 4;
        return true;
    }
    static bool IsStr32(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        if ((w & 0xFFC00000u) != 0xB9000000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF) * 4;
        return true;
    }
    static bool IsLdrb(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        uint m = w & 0xFFC00000u;
        if (m != 0x39400000u && m != 0x39C00000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF);
        return true;
    }
    static bool IsLdrh(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        if ((w & 0xFFC00000u) != 0x79400000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF) * 2;
        return true;
    }
    static bool IsStrh(uint w, out int rt, out int rn, out int off)
    {
        rt = rn = off = 0;
        if ((w & 0xFFC00000u) != 0x79000000u) return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F); off = (int)((w >> 10) & 0xFFF) * 2;
        return true;
    }
    static bool IsLdrStrReg(uint w, out bool isLoad, out int bits, out int rt, out int rn, out int rm, out int opt, out int sbit)
    {
        isLoad = false; bits = 0; rt = rn = rm = opt = sbit = 0;
        uint m = w & 0xFFE00C00u;
        if ((m & 0xFFE00800u) == 0xF8600800u) { isLoad = true; bits = 64; }
        else if ((m & 0xFFE00800u) == 0xF8200800u) { isLoad = false; bits = 64; }
        else if ((m & 0xFFE00800u) == 0xB8600800u) { isLoad = true; bits = 32; }
        else if ((m & 0xFFE00800u) == 0xB8200800u) { isLoad = false; bits = 32; }
        else return false;
        rt = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        rm = (int)((w >> 16) & 0x1F); opt = (int)((w >> 13) & 7); sbit = (int)((w >> 12) & 1);
        return true;
    }
    static bool IsAddReg(uint w, out int sf, out int rd, out int rn, out int rm, out int amount)
    {
        sf = 0; rd = rn = rm = amount = 0;
        if ((w & 0x7F200000u) != 0x0B000000u) return false;
        sf = (w & 0x80000000u) != 0 ? 64 : 32;
        if (((w >> 22) & 3) != 0) return false;
        rd = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        rm = (int)((w >> 16) & 0x1F); amount = (int)((w >> 10) & 0x3F);
        return true;
    }
    static bool IsBl(uint w) { return (w & 0xFC000000u) == 0x94000000u; }
    static bool IsB(uint w) { return (w & 0xFC000000u) == 0x14000000u; }
    static bool IsBr(uint w) { return (w & 0xFFFFFC1Fu) == 0xD61F0000u; }
    static bool IsRet(uint w) { return w == 0xD65F03C0u; }

    static bool IsUbfm(uint w, out int sf, out int immr, out int imms, out int rd, out int rn)
    {
        sf = 0; immr = 0; imms = 0; rd = 0; rn = 0;
        uint m = w & 0x7F800000u;
        if (m == 0x53000000u) sf = 32;
        else if (m == 0xD3400000u) sf = 64;
        else return false;
        immr = (int)((w >> 16) & 0x3F);
        imms = (int)((w >> 10) & 0x3F);
        rd = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        return true;
    }

    static bool IsAndImm32(uint w, out int rd, out int rn, out int immr, out int imms)
    {
        rd = rn = immr = imms = 0;
        if ((w & 0x1F800000u) != 0x12000000u) return false;
        rd = (int)(w & 0x1F); rn = (int)((w >> 5) & 0x1F);
        immr = (int)((w >> 16) & 0x3F); imms = (int)((w >> 10) & 0x3F);
        return true;
    }

    static string Mv(uint w)
    {
        uint m = w & 0x7F800000u;
        if (m == 0x52800000u || m == 0xD2800000u)
        {
            bool is64 = (w & 0x80000000u) != 0;
            return string.Format("mov {0}{1}, #0x{2:x}", is64 ? "x" : "w", (int)(w & 0x1F),
                (long)((w >> 5) & 0xFFFF) << (int)(((w >> 21) & 3) * 16));
        }
        if (m == 0x72800000u || m == 0xF2800000u)
        {
            bool is64 = (w & 0x80000000u) != 0;
            return string.Format("movk {0}{1}, #0x{2:x}, lsl {3}", is64 ? "x" : "w", (int)(w & 0x1F),
                (long)((w >> 5) & 0xFFFF), (int)(((w >> 21) & 3) * 16));
        }
        if (m == 0x12800000u || m == 0x92800000u)
        {
            bool is64 = (w & 0x80000000u) != 0;
            long v = (long)((w >> 5) & 0xFFFF) << (int)(((w >> 21) & 3) * 16);
            return string.Format("movn {0}{1}, #0x{2:x}", is64 ? "x" : "w", (int)(w & 0x1F), ~v);
        }
        return null;
    }

    static string Decode(uint w, ulong pc)
    {
        if (IsAdrp(w)) return string.Format("adrp x{0}, 0x{1:x}", (int)(w & 0x1F), AdrpTarget(w, pc));
        int rd, rn, off; long imm; bool sh;
        if (IsAddImm64(w, out rd, out rn, out imm, out sh))
            return string.Format("add {0}x{1}, x{2}, #0x{3:x}{4}", (w & 0x80000000u) != 0 ? "" : "(w)",
                rd, rn, imm, sh ? ", lsl 12" : "");
        if (IsLdr64(w, out rd, out rn, out off)) return string.Format("ldr x{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsStr64(w, out rd, out rn, out off)) return string.Format("str x{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsLdr32(w, out rd, out rn, out off)) return string.Format("ldr w{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsStr32(w, out rd, out rn, out off)) return string.Format("str w{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsLdrb(w, out rd, out rn, out off)) return string.Format("ldrb w{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsLdrh(w, out rd, out rn, out off)) return string.Format("ldrh w{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        if (IsStrh(w, out rd, out rn, out off)) return string.Format("strh w{0}, [x{1}, #0x{2:x}]", rd, rn, off);
        bool isLoad; int bits, rm, s2, opt;
        if (IsLdrStrReg(w, out isLoad, out bits, out rd, out rn, out rm, out opt, out s2))
            return string.Format("{0} {1}{2}, [x{3}, x{4}, opt={5}, s={6}]", isLoad ? "ldr" : "str",
                bits == 64 ? "x" : "w", rd, rn, rm, opt, s2);
        int sf, immr, imms;
        if (IsUbfm(w, out sf, out immr, out imms, out rd, out rn))
        {
            int width = imms + 1 - immr;
            if (imms == sf - 1 && immr == 0)
                return string.Format("and {0}{1}, {0}{2}, ALL", sf == 64 ? "x" : "w", rd, rn);
            if (imms == sf - 1)
                return string.Format("lsr {0}{1}, {0}{2}, #{3}", sf == 64 ? "x" : "w", rd, rn, immr);
            if (immr == 0)
                return string.Format("and/ubfx {0}{1}, {0}{2}, #{3}bits mask=0x{4:x}",
                    sf == 64 ? "x" : "w", rd, rn, imms + 1, (1UL << (imms + 1)) - 1);
            return string.Format("ubfx {0}{1}, {0}{2}, #{3}, #{4}", sf == 64 ? "x" : "w", rd, rn, immr, width);
        }
        string mv = Mv(w);
        if (mv != null) return mv;
        if (IsBl(w)) { long i = SignExt((long)(w & 0x3FFFFFF), 26); return string.Format("bl 0x{0:x}", (ulong)((long)pc + i * 4)); }
        if (IsB(w)) { long i = SignExt((long)(w & 0x3FFFFFF), 26); return string.Format("b 0x{0:x}", (ulong)((long)pc + i * 4)); }
        if ((w & 0xFF000010u) == 0x54000000u)
        {
            long i = SignExt((long)((w >> 5) & 0x7FFFF), 19);
            int cond = (int)(w & 0xF);
            string[] cc = new string[] { "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv" };
            return string.Format("b.{0} 0x{1:x}", cc[cond], (ulong)((long)pc + i * 4));
        }
        if ((w & 0x7E000000u) == 0x34000000u)
        {
            bool is64 = (w & 0x80000000u) != 0;
            bool nz = (w & 0x01000000u) != 0;
            long i = SignExt((long)((w >> 5) & 0x7FFFF), 19);
            return string.Format("{0} {1}{2}, 0x{3:x}", nz ? "cbnz" : "cbz", is64 ? "x" : "w",
                (int)(w & 0x1F), (ulong)((long)pc + i * 4));
        }
        if ((w & 0x7E000000u) == 0x36000000u)
        {
            bool nz = (w & 0x01000000u) != 0;
            int bit = (int)(((w >> 31) & 1) | (((w >> 19) & 0x1F) << 5));
            long i = SignExt((long)((w >> 5) & 0x3FFF), 14);
            return string.Format("{0} x{1}, #{2}, 0x{3:x}", nz ? "tbnz" : "tbz", (int)(w & 0x1F), bit,
                (ulong)((long)pc + i * 4));
        }
        if (w == 0xD65F03C0u) return "ret";
        if (w == 0xD503201Fu) return "nop";
        if (IsBr(w)) return "br x" + (int)((w >> 5) & 0x1F);
        if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) return "blr x" + (int)((w >> 5) & 0x1F);
        return string.Format(".word 0x{0:x8}", w);
    }

    static long SignExt(long v, int bits)
    {
        long m = 1L << (bits - 1);
        v &= (1L << bits) - 1;
        return (v ^ m) - m;
    }

    static void DumpWindow(ulong pc, int back, int fwd)
    {
        for (int k = -back; k <= fwd; k++)
        {
            ulong a = pc + (ulong)(4 * k);
            long f = FileOf(a);
            if (f < 0) continue;
            uint wk = U32(f);
            string line = string.Format("   {0}0x{1:x}  {2}", k == 0 ? ">> " : "   ", a, Decode(wk, a));
            if (IsAdrp(wk))
            {
                ulong t = AdrpTarget(wk, a);
                long f2 = FileOf(a + 4);
                if (f2 >= 0)
                {
                    int rd2, rn2, off2; long imm2; bool sh2;
                    if (IsAddImm64(U32(f2), out rd2, out rn2, out imm2, out sh2) && rn2 == (int)(wk & 0x1F) && rd2 == rn2)
                        t = (ulong)((long)t + imm2);
                }
                line += string.Format("   ; ->0x{0:x}", t);
                string s = ReadCString(t);
                if (s != null) line += " \"" + (s.Length > 50 ? s.Substring(0, 50) : s) + "\"";
            }
            Console.WriteLine(line);
        }
    }

    // ---------------- shift ----------------
    static void CmdShift(string bin, int minCount)
    {
        Load(bin);
        long s0 = FileOf(TEXT_LO), s1 = FileOf(TEXT_LO + TEXT_SIZE - 4);
        var hist = new Dictionary<string, int>();
        var samp = new Dictionary<string, ulong>();
        for (long i = s0; i + 4 < s1; i += 4)
        {
            uint w = U32(i);
            ulong pc = TEXT_LO + (ulong)(i - s0);
            int sf, immr, imms, rd, rn;
            if (IsUbfm(w, out sf, out immr, out imms, out rd, out rn))
            {
                string key = string.Format("ubfm{0} immr={1} imms={2} ({3})", sf, immr, imms, Decode(w, pc));
                if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                hist[key] = hist[key] + 1;
            }
            int r2, i2, i3;
            if (IsAndImm32(w, out rd, out rn, out i2, out i3))
            {
                string key = string.Format("and32 immr={0} imms={1}", i2, i3);
                if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                hist[key] = hist[key] + 1;
            }
        }
        var list = new List<KeyValuePair<string, int>>(hist);
        list.Sort(delegate (KeyValuePair<string, int> a, KeyValuePair<string, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("=== ubfm/and histogram (kinds {0}) ===", list.Count);
        foreach (var kv in list)
        {
            if (kv.Value < minCount) continue;
            Console.WriteLine("  {0,-60} x{1,-7} first=0x{2:x}", kv.Key, kv.Value, samp[kv.Key]);
        }
    }

    // ---------------- resolve ----------------
    static void CmdResolve(string bin, int maxPrint)
    {
        Load(bin);
        long s0 = FileOf(TEXT_LO), s1 = FileOf(TEXT_LO + TEXT_SIZE - 4);
        var hist = new Dictionary<string, int>();
        var samp = new Dictionary<string, ulong>();
        for (long i = s0; i + 4 < s1; i += 4)
        {
            uint w0 = U32(i);
            if (!IsAdrp(w0)) continue;
            ulong pc0 = TEXT_LO + (ulong)(i - s0);
            int base_reg = (int)(w0 & 0x1F);
            var kind = new int[32];
            var val = new ulong[32];
            var idxsh = new int[32];
            kind[base_reg] = 1; val[base_reg] = AdrpTarget(w0, pc0);
            for (int k = 1; k <= 22; k++)
            {
                ulong pc = pc0 + (ulong)(4 * k);
                long f = FileOf(pc);
                if (f < 0) break;
                uint w = U32(f);
                if (IsBl(w) || IsBr(w) || IsRet(w) || IsB(w)) break;
                int rd, rn, off; long imm; bool sh;
                if (IsAddImm64(w, out rd, out rn, out imm, out sh) && rd == rn && kind[rn] == 1)
                { val[rn] = (ulong)((long)val[rn] + imm); continue; }

                int sfa, rma, amt;
                if (IsAddReg(w, out sfa, out rd, out rn, out rma, out amt) && kind[rn] != 0 && amt <= 4)
                { kind[rd] = 3; val[rd] = val[rn]; idxsh[rd] = amt; continue; }

                bool isLoad; int bits, rm, s2, opt;
                if (IsLdrStrReg(w, out isLoad, out bits, out rd, out rn, out rm, out opt, out s2))
                {
                    if (kind[rn] != 0)
                    {
                        string key = string.Format("{0}{1} base=0x{2:x} opt={3} s={4} kind={5}",
                            isLoad ? "ldr" : "str", bits, val[rn], opt, s2, kind[rn]);
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                    }
                    kind[rd] = 0; continue;
                }
                if (IsLdr64(w, out rd, out rn, out off))
                {
                    if (kind[rn] == 1) { kind[rd] = 2; val[rd] = val[rn] + (ulong)off; }
                    else if (kind[rn] == 3)
                    {
                        string key = string.Format("ADDIX base=0x{0:x} scale={1} then ldr x,[..,#0x{2:x}]",
                            val[rn], idxsh[rn], off);
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                        kind[rd] = 4;
                    }
                    else kind[rd] = 0;
                    continue;
                }
                if (IsStr64(w, out rd, out rn, out off))
                {
                    if (kind[rn] == 3)
                    {
                        string key = string.Format("ADDIX base=0x{0:x} scale={1} then str x,[..,#0x{2:x}]",
                            val[rn], idxsh[rn], off);
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                    }
                    continue;
                }
                if (IsLdr32(w, out rd, out rn, out off) || IsStr32(w, out rd, out rn, out off)) { kind[rd] = 0; continue; }
            }
        }
        var list = new List<KeyValuePair<string, int>>(hist);
        list.Sort(delegate (KeyValuePair<string, int> a, KeyValuePair<string, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("=== Blocks[] candidate shapes (kinds {0}) ===", list.Count);
        int n = 0;
        foreach (var kv in list)
        {
            Console.WriteLine("  {0,-72} x{1,-6} first=0x{2:x}", kv.Key, kv.Value, samp[kv.Key]);
            if (++n >= maxPrint) break;
        }
    }

    // ---------------- globals ----------------
    static void CmdGlobals(string bin, string offCsv, int maxPrint)
    {
        Load(bin);
        var want = new Dictionary<int, int>();
        if (offCsv != "-") foreach (var s in offCsv.Split(',')) want[(int)H(s.Trim())] = 1;
        long s0 = FileOf(TEXT_LO), s1 = FileOf(TEXT_LO + TEXT_SIZE - 4);
        var hist = new Dictionary<string, int>();
        var samp = new Dictionary<string, ulong>();
        for (long i = s0; i + 4 < s1; i += 4)
        {
            uint w0 = U32(i);
            if (!IsAdrp(w0)) continue;
            ulong pc0 = TEXT_LO + (ulong)(i - s0);
            int adrpRd = (int)(w0 & 0x1F);
            ulong cur = AdrpTarget(w0, pc0);
            if (cur + 0x1000 < DATA_LO) continue;
            for (int k = 1; k <= 6; k++)
            {
                ulong pc = pc0 + (ulong)(4 * k);
                long f = FileOf(pc);
                if (f < 0) break;
                uint w = U32(f);
                int rd, rn, off; long imm; bool sh;
                if (IsAddImm64(w, out rd, out rn, out imm, out sh) && rn == adrpRd && rd == adrpRd)
                { cur = (ulong)((long)cur + imm); continue; }
                if (IsLdr64(w, out rd, out rn, out off) && rn == adrpRd)
                {
                    ulong slot = cur + (ulong)off;
                    if (want.Count == 0 && (slot < DATA_LO || slot > DATA_HI)) break;
                    string key = string.Format("G=0x{0:x} +0x{1:x} ldr64 slot=0x{2:x}", cur, off, slot);
                    if (want.Count == 0 || want.ContainsKey(off))
                    {
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                    }
                    break;
                }
                if (IsStr64(w, out rd, out rn, out off) && rn == adrpRd)
                {
                    ulong slot = cur + (ulong)off;
                    if (want.Count == 0 && (slot < DATA_LO || slot > DATA_HI)) break;
                    string key = string.Format("G=0x{0:x} +0x{1:x} str64 slot=0x{2:x}", cur, off, slot);
                    if (want.Count == 0 || want.ContainsKey(off))
                    {
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                    }
                    break;
                }
                if (IsLdr32(w, out rd, out rn, out off) && rn == adrpRd)
                {
                    string key = string.Format("G=0x{0:x} +0x{1:x} ldr32 slot=0x{2:x}", cur, off, cur + (ulong)off);
                    if (want.ContainsKey(off))
                    {
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                    }
                    break;
                }
                if (IsStr32(w, out rd, out rn, out off) && rn == adrpRd)
                {
                    string key = string.Format("G=0x{0:x} +0x{1:x} str32 slot=0x{2:x}", cur, off, cur + (ulong)off);
                    if (want.ContainsKey(off))
                    {
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc; }
                        hist[key] = hist[key] + 1;
                    }
                    break;
                }
            }
        }
        var list = new List<KeyValuePair<string, int>>(hist);
        list.Sort(delegate (KeyValuePair<string, int> a, KeyValuePair<string, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("=== global field sites (kinds {0}) ===", list.Count);
        int n = 0;
        foreach (var kv in list)
        {
            Console.WriteLine("  {0,-58} x{1,-6} first=0x{2:x}", kv.Key, kv.Value, samp[kv.Key]);
            if (++n >= maxPrint) break;
        }
    }

    // ---------------- fn16 ----------------
    static void CmdFn16(string bin, int shift, int maxWin)
    {
        Load(bin);
        long s0 = FileOf(TEXT_LO), s1 = FileOf(TEXT_LO + TEXT_SIZE - 4);
        int printed = 0;
        ulong lastEnd = 0;
        for (long i = s0; i + 4 < s1; i += 4)
        {
            uint w = U32(i);
            int sf, immr, imms, rd, rn;
            if (!IsUbfm(w, out sf, out immr, out imms, out rd, out rn)) continue;
            if (sf != 32 || immr != shift || imms != 31) continue;
            ulong pc = TEXT_LO + (ulong)(i - s0);
            if (pc < lastEnd) continue;
            bool hasAdrp = false;
            for (int k = -12; k <= 14; k++)
            {
                long f = FileOf(pc + (ulong)(4 * k));
                if (f < 0) continue;
                if (IsAdrp(U32(f))) { hasAdrp = true; break; }
            }
            if (!hasAdrp) continue;
            Console.WriteLine("--- lsr w{0}, w{1}, #{2} @ 0x{3:x} ---", rd, rn, shift, pc);
            DumpWindow(pc, 12, 14);
            lastEnd = pc + 4 * 14;
            if (++printed >= maxWin) break;
        }
        Console.WriteLine("windows = {0}", printed);
    }

    // ---------------- allocblk ----------------
    static void CmdAllocBlk(string bin, ulong wantImm, int half, int maxWin)
    {
        Load(bin);
        long s0 = FileOf(TEXT_LO), s1 = FileOf(TEXT_LO + TEXT_SIZE - 4);
        int printed = 0;
        ulong lastEnd = 0;
        for (long i = s0; i + 4 < s1; i += 4)
        {
            uint w = U32(i);
            uint m = w & 0x7F800000u;
            if (m != 0x52800000u && m != 0xD2800000u) continue;
            int hw = (int)((w >> 21) & 3);
            ulong v = (ulong)((w >> 5) & 0xFFFF) << (hw * 16);
            if (v != wantImm) continue;
            ulong pc = TEXT_LO + (ulong)(i - s0);
            if (pc < lastEnd) continue;
            Console.WriteLine("--- mov w{0}, #0x{1:x} @ 0x{2:x} ---", (int)(w & 0x1F), v, pc);
            DumpWindow(pc, half, half);
            lastEnd = pc + (ulong)(4 * half);
            if (++printed >= maxWin) break;
        }
        Console.WriteLine("windows = {0}", printed);
    }

    static void CmdCtx(string bin, ulong va, int count)
    {
        Load(bin);
        for (int i = 0; i < count; i++)
        {
            ulong a = va + (ulong)(4 * i);
            long f = FileOf(a);
            if (f < 0) { Console.WriteLine("0x{0:x}  <unmapped>", a); continue; }
            uint w = U32(f);
            string line = string.Format("0x{0:x}  {1:x8}  {2}", a, w, Decode(w, a));
            if (IsAdrp(w))
            {
                ulong t = AdrpTarget(w, a);
                long f2 = FileOf(a + 4);
                if (f2 >= 0)
                {
                    int rd2, rn2, off2; long imm2; bool sh2;
                    if (IsAddImm64(U32(f2), out rd2, out rn2, out imm2, out sh2) && rn2 == (int)(w & 0x1F) && rd2 == rn2)
                        t = (ulong)((long)t + imm2);
                }
                string s = ReadCString(t);
                if (s != null) line += "   ; \"" + (s.Length > 70 ? s.Substring(0, 70) + "..." : s) + "\"";
            }
            Console.WriteLine(line);
        }
    }

    // ---------------- ptr2 ----------------
    // adrp G ; ldr xA,[G,#off]  (global POINTER)  then within N insns  ldr/str [xA,#imm]
    // -> reports the pointer variable address (G+off) and the field offset used on the pointee.
    static void CmdPtr2(string bin, string offCsv, int maxPrint)
    {
        Load(bin);
        var want = new Dictionary<int, int>();
        if (offCsv != "-") foreach (var s in offCsv.Split(',')) want[(int)H(s.Trim())] = 1;
        long s0 = FileOf(TEXT_LO), s1 = FileOf(TEXT_LO + TEXT_SIZE - 4);
        var hist = new Dictionary<string, int>();
        var samp = new Dictionary<string, ulong>();
        for (long i = s0; i + 4 < s1; i += 4)
        {
            uint w0 = U32(i);
            if (!IsAdrp(w0)) continue;
            ulong pc0 = TEXT_LO + (ulong)(i - s0);
            int adrpRd = (int)(w0 & 0x1F);
            ulong cur = AdrpTarget(w0, pc0);
            if (cur + 0x1000 < DATA_LO) continue;
            // level 1: find the ldr that materialises the global pointer
            int ptrReg = -1; ulong ptrVar = 0; int k0 = 0;
            for (int k = 1; k <= 6; k++)
            {
                uint w = U32(FileOf(pc0 + (ulong)(4 * k)));
                int rd, rn, off; long imm; bool sh;
                if (IsAddImm64(w, out rd, out rn, out imm, out sh) && rn == adrpRd && rd == adrpRd)
                { cur = (ulong)((long)cur + imm); continue; }
                if (IsLdr64(w, out rd, out rn, out off) && rn == adrpRd)
                { ptrReg = rd; ptrVar = cur + (ulong)off; k0 = k; }
                break;
            }
            if (ptrReg < 0) continue;
            if (ptrVar < DATA_LO || ptrVar > DATA_HI) continue;
            // level 2
            for (int k = k0 + 1; k <= k0 + 10; k++)
            {
                uint w = U32(FileOf(pc0 + (ulong)(4 * k)));
                if (IsBl(w) || IsB(w) || IsBr(w) || IsRet(w)) break;
                int rd, rn, off;
                bool isLoad; int bits, rm, opt, s2;
                if (IsLdrStrReg(w, out isLoad, out bits, out rd, out rn, out rm, out opt, out s2) && rn == ptrReg)
                {
                    string key = string.Format("PTRVAR=0x{0:x} -> reg-idx opt={1} s={2} bits={3}", ptrVar, opt, s2, bits);
                    if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc0 + (ulong)(4 * k); }
                    hist[key] = hist[key] + 1;
                    break;
                }
                if ((IsLdr64(w, out rd, out rn, out off) || IsStr64(w, out rd, out rn, out off) ||
                     IsLdr32(w, out rd, out rn, out off) || IsStr32(w, out rd, out rn, out off)) && rn == ptrReg)
                {
                    if (want.Count == 0 || want.ContainsKey(off))
                    {
                        string key = string.Format("PTRVAR=0x{0:x} -> +0x{1:x}", ptrVar, off);
                        if (!hist.ContainsKey(key)) { hist[key] = 0; samp[key] = pc0 + (ulong)(4 * k); }
                        hist[key] = hist[key] + 1;
                    }
                    break;
                }
            }
        }
        var list = new List<KeyValuePair<string, int>>(hist);
        list.Sort(delegate (KeyValuePair<string, int> a, KeyValuePair<string, int> b) { return b.Value.CompareTo(a.Value); });
        Console.WriteLine("=== pointer-global deref (kinds {0}) ===", list.Count);
        int n = 0;
        foreach (var kv in list)
        {
            Console.WriteLine("  {0,-58} x{1,-6} first=0x{2:x}", kv.Key, kv.Value, samp[kv.Key]);
            if (++n >= maxPrint) break;
        }
    }

    static int Main(string[] args)
    {
        try
        {
            if (args.Length < 3) { Console.WriteLine("usage: PoolScan <cmd> <bin> ..."); return 2; }
            string cmd = args[0], bin = args[1];
            switch (cmd)
            {
                case "shift": CmdShift(bin, args.Length > 2 ? int.Parse(args[2]) : 1); break;
                case "resolve": CmdResolve(bin, args.Length > 2 ? int.Parse(args[2]) : 60); break;
                case "globals": CmdGlobals(bin, args[2], args.Length > 3 ? int.Parse(args[3]) : 80); break;
                case "ptr2": CmdPtr2(bin, args[2], args.Length > 3 ? int.Parse(args[3]) : 60); break;
                case "fn16": CmdFn16(bin, int.Parse(args[2]), int.Parse(args[3])); break;
                case "allocblk": CmdAllocBlk(bin, H(args[2]), int.Parse(args[3]), int.Parse(args[4])); break;
                case "ctx": CmdCtx(bin, H(args[2]), int.Parse(args[3])); break;
                default: Console.WriteLine("unknown cmd " + cmd); return 2;
            }
            return 0;
        }
        catch (Exception e)
        {
            Console.WriteLine("ERROR: " + e.Message);
            Console.WriteLine(e.StackTrace);
            return 1;
        }
    }
}

