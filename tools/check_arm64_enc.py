import sys

def enc_b(frm, to):
    off = to - frm
    if off % 4 != 0:
        return None, "offset not 4-byte aligned"
    imm26 = (off >> 2) & 0x03FFFFFF
    if not (-(1 << 27) <= off <= (1 << 27) - 4):
        return None, "out of +/-128MB range (off=%d)" % off
    insn = 0x14000000 | imm26
    return insn, "off=%d imm26=0x%07x" % (off, imm26)

def enc_adrp_add_br(frm, to):
    # ADRP x16, page(to)
    page_delta = (to & ~0xFFF) - (frm & ~0xFFF)
    if page_delta % 4096 != 0:
        return None, "page delta not page aligned"
    pages = page_delta >> 12
    if not (-(1 << 20) <= pages <= (1 << 20) - 1):
        return None, "ADRP out of +/-4GB range"
    immlo = pages & 0x3
    immhi = (pages >> 2) & 0x7FFFF
    adrp = 0x90000000 | (immlo << 29) | (immhi << 5) | 16   # Rd=16 (x16)
    # ADD x16, x16, #imm12
    off = to & 0xFFF
    add = 0x91000000 | (off << 10) | (16 << 5) | 16          # Rd=16, Rn=16
    br = 0xD61F0200                                          # br x16
    nop = 0xD503201F
    return (adrp, add, br, nop), "pages=%d immlo=%d immhi=0x%x off=0x%x" % (pages, immlo, immhi, off)

print("=== B 指令（近距离）===")
for frm, to in [(0x100004000, 0x100010000), (0x100010000, 0x100004000), (0x100000000, 0x108000000)]:
    insn, note = enc_b(frm, to)
    print("  from=0x%x to=0x%x -> %s   [%s]" % (frm, to, ("0x%08x" % insn) if insn else "不可编码", note))

print("\n=== ADRP+ADD+BR（远距离/兜底）===")
for frm, to in [(0x100004000, 0x100010000), (0x100000000, 0x1F0000000)]:
    r, note = enc_adrp_add_br(frm, to)
    if r:
        print("  from=0x%x to=0x%x -> adrp=0x%08x add=0x%08x br=0x%08x nop=0x%08x   [%s]" % (frm, to, r[0], r[1], r[2], r[3], note))
    else:
        print("  from=0x%x to=0x%x -> 不可编码 [%s]" % (frm, to, note))

print("\n=== 解码自检（把 B 再解回来）===")
insn, _ = enc_b(0x100004000, 0x100010000)
imm26 = insn & 0x03FFFFFF
if imm26 & (1 << 25):
    imm26 -= (1 << 26)
target = 0x100004000 + (imm26 << 2)
print("  解码 target = 0x%x  (期望 0x100010000)  %s" % (target, "OK" if target == 0x100010000 else "FAIL"))
