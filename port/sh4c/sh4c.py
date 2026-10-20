"""SH-4 -> C static translator for Soulcalibur (Dreamcast), milestone M0.

usage: python -I sh4c.py <out.c> <entry> [entry ...]
Reads sc/extracted/1ST_READ.BIN (loaded at 0x8C010000) and sc/port/functions.json (Ghidra
export: function entry, name, body ranges). Emits one C function per entry:

    void f_<entry>(SH4 *c);

Semantics follow Flycast's SH-4 interpreter (core/hw/sh4/interpr/sh4_opcodes.cpp), which is
also the oracle used to validate the output. Unknown opcodes become sh4_unimpl() calls.
Floating point: each FPU op calls a helper in sh4rt.h that copies Flycast sh4_fpu.cpp, checking FPSCR.PR/SZ at run time.
"""
import json
import os
import struct
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
BASE = 0x8C010000
IMG = open(os.path.join(SC, "extracted", "1ST_READ.BIN"), "rb").read()
# read-only code blocks whose literal pools can be folded to constants
RO = [(0x8C010000, 0x8C1036E0), (0x8C21AF20, 0x8C245620)]


def ro(addr):
    return any(lo <= addr < hi for lo, hi in RO)


def u16(addr):
    return struct.unpack_from("<H", IMG, addr - BASE)[0]


def u32(addr):
    return struct.unpack_from("<I", IMG, addr - BASE)[0]


def s8(v):
    return v - 256 if v & 0x80 else v


def s12(v):
    return v - 4096 if v & 0x800 else v


# ---- decoding -------------------------------------------------------------------------
# pattern: 16 chars of 0/1/n/m/i/d. Returns (name, fields)
PATTERNS = [
    ("0000000000001001", "nop"), ("0000000000001011", "rts"), ("0000000000001000", "clrt"),
    ("0000000000011000", "sett"), ("0000000000011001", "div0u"), ("0000000000101000", "clrmac"),
    ("0000000001001000", "clrs"), ("0000000001011000", "sets"),
    ("0110nnnnmmmm0011", "mov"), ("1110nnnniiiiiiii", "mov_imm"),
    ("1001nnnndddddddd", "movw_pc"), ("1101nnnndddddddd", "movl_pc"), ("11000111dddddddd", "mova"),
    ("0010nnnnmmmm0000", "movb_st"), ("0010nnnnmmmm0001", "movw_st"), ("0010nnnnmmmm0010", "movl_st"),
    ("0110nnnnmmmm0000", "movb_ld"), ("0110nnnnmmmm0001", "movw_ld"), ("0110nnnnmmmm0010", "movl_ld"),
    ("0010nnnnmmmm0100", "movb_dec"), ("0010nnnnmmmm0101", "movw_dec"), ("0010nnnnmmmm0110", "movl_dec"),
    ("0110nnnnmmmm0100", "movb_inc"), ("0110nnnnmmmm0101", "movw_inc"), ("0110nnnnmmmm0110", "movl_inc"),
    ("10000000nnnndddd", "movb_st_r0d"), ("10000001nnnndddd", "movw_st_r0d"), ("0001nnnnmmmmdddd", "movl_st_d"),
    ("10000100mmmmdddd", "movb_ld_r0d"), ("10000101mmmmdddd", "movw_ld_r0d"), ("0101nnnnmmmmdddd", "movl_ld_d"),
    ("0000nnnnmmmm0100", "movb_st_r0x"), ("0000nnnnmmmm0101", "movw_st_r0x"), ("0000nnnnmmmm0110", "movl_st_r0x"),
    ("0000nnnnmmmm1100", "movb_ld_r0x"), ("0000nnnnmmmm1101", "movw_ld_r0x"), ("0000nnnnmmmm1110", "movl_ld_r0x"),
    ("11000000dddddddd", "movb_st_gbr"), ("11000001dddddddd", "movw_st_gbr"), ("11000010dddddddd", "movl_st_gbr"),
    ("11000100dddddddd", "movb_ld_gbr"), ("11000101dddddddd", "movw_ld_gbr"), ("11000110dddddddd", "movl_ld_gbr"),
    ("0000nnnn00101001", "movt"),
    ("0011nnnnmmmm1100", "add"), ("0111nnnniiiiiiii", "add_imm"), ("0011nnnnmmmm1110", "addc"),
    ("0011nnnnmmmm1000", "sub"), ("0011nnnnmmmm1010", "subc"), ("0110nnnnmmmm1011", "neg"),
    ("0110nnnnmmmm1010", "negc"), ("0010nnnnmmmm1001", "and"), ("11001001iiiiiiii", "and_imm"),
    ("0010nnnnmmmm1011", "or"), ("11001011iiiiiiii", "or_imm"), ("0010nnnnmmmm1010", "xor"),
    ("11001010iiiiiiii", "xor_imm"), ("0110nnnnmmmm0111", "not"),
    ("0010nnnnmmmm1000", "tst"), ("11001000iiiiiiii", "tst_imm"),
    ("10001000iiiiiiii", "cmpeq_imm"), ("0011nnnnmmmm0000", "cmpeq"), ("0011nnnnmmmm0010", "cmphs"),
    ("0011nnnnmmmm0011", "cmpge"), ("0011nnnnmmmm0110", "cmphi"), ("0011nnnnmmmm0111", "cmpgt"),
    ("0100nnnn00010101", "cmppl"), ("0100nnnn00010001", "cmppz"), ("0010nnnnmmmm1100", "cmpstr"),
    ("0010nnnnmmmm0111", "div0s"), ("0011nnnnmmmm0100", "div1"),
    ("0110nnnnmmmm1100", "extub"), ("0110nnnnmmmm1101", "extuw"), ("0110nnnnmmmm1110", "extsb"),
    ("0110nnnnmmmm1111", "extsw"), ("0110nnnnmmmm1000", "swapb"), ("0110nnnnmmmm1001", "swapw"),
    ("0010nnnnmmmm1101", "xtrct"),
    ("0100nnnn00000000", "shll"), ("0100nnnn00000001", "shlr"), ("0100nnnn00100000", "shal"),
    ("0100nnnn00100001", "shar"), ("0100nnnn00001000", "shll2"), ("0100nnnn00001001", "shlr2"),
    ("0100nnnn00011000", "shll8"), ("0100nnnn00011001", "shlr8"), ("0100nnnn00101000", "shll16"),
    ("0100nnnn00101001", "shlr16"), ("0100nnnnmmmm1100", "shad"), ("0100nnnnmmmm1101", "shld"),
    ("0100nnnn00000100", "rotl"), ("0100nnnn00000101", "rotr"), ("0100nnnn00100100", "rotcl"),
    ("0100nnnn00100101", "rotcr"), ("0100nnnn00010000", "dt"),
    ("0000nnnnmmmm0111", "mull"), ("0010nnnnmmmm1111", "mulsw"), ("0010nnnnmmmm1110", "muluw"),
    ("0011nnnnmmmm1101", "dmulsl"), ("0011nnnnmmmm0101", "dmulul"),
    ("10001001dddddddd", "bt"), ("10001011dddddddd", "bf"), ("10001101dddddddd", "bts"),
    ("10001111dddddddd", "bfs"), ("1010dddddddddddd", "bra"), ("1011dddddddddddd", "bsr"),
    ("0000nnnn00100011", "braf"), ("0000nnnn00000011", "bsrf"), ("0100nnnn00101011", "jmp"),
    ("0100nnnn00001011", "jsr"),
    ("0000nnnn00101010", "sts_pr"), ("0100nnnn00100010", "stsl_pr"), ("0100nnnn00101010", "lds_pr"),
    ("0100nnnn00100110", "ldsl_pr"), ("0000nnnn00001010", "sts_mach"), ("0000nnnn00011010", "sts_macl"),
    ("0100nnnn00001010", "lds_mach"), ("0100nnnn00011010", "lds_macl"), ("0000nnnn00010010", "stc_gbr"),
    ("0100nnnn00011110", "ldc_gbr"), ("0100nnnn00010010", "stsl_macl"), ("0100nnnn00010110", "ldsl_macl"),
    ("0100nnnn00000010", "stsl_mach"), ("0100nnnn00000110", "ldsl_mach"),
    ("0000nnnn10000011", "pref"),
    ("0000nnnnmmmm1111", "macl_"), ("0100nnnnmmmm1111", "macw"), ("0100nnnn00011011", "tasb"),
    ("0000nnnn11000011", "movcal"), ("0000nnnn10010011", "ocbi"), ("0000nnnn10100011", "ocbp"),
    ("0000nnnn10110011", "ocbwb"), ("0011nnnnmmmm1111", "addv"), ("0011nnnnmmmm1011", "subv"),
    ("11001101iiiiiiii", "andb_gbr"), ("11001111iiiiiiii", "orb_gbr"), ("11001110iiiiiiii", "xorb_gbr"),
    ("11001100iiiiiiii", "tstb_gbr"),
    ("0000nnnn00000010", "stc_sr"), ("0100nnnn00001110", "ldc_sr"), ("0100nnnn00000011", "stcl_sr"),
    ("0100nnnn00000111", "ldcl_sr"), ("0100nnnn00010011", "stcl_gbr"), ("0100nnnn00010111", "ldcl_gbr"),
    ("0000nnnn00100010", "stc_vbr"), ("0100nnnn00101110", "ldc_vbr"), ("0100nnnn00100011", "stcl_vbr"),
    ("0100nnnn00100111", "ldcl_vbr"),
    # ---- FPU (specific encodings first) ----
    ("1111101111111101", "frchg"), ("1111001111111101", "fschg"), ("1111nn0111111101", "ftrv"),
    ("1111nnn011111101", "fsca"), ("1111nnmm11101101", "fipr"),
    ("1111nnnn01011101", "fabs"), ("1111nnnn01111101", "fsrra"), ("1111nnnn10111101", "fcnvds"),
    ("1111nnnn10101101", "fcnvsd"), ("1111nnnn10001101", "fldi0"), ("1111nnnn10011101", "fldi1"),
    ("1111nnnn00011101", "flds"), ("1111nnnn00001101", "fsts"), ("1111nnnn00101101", "float"),
    ("1111nnnn01001101", "fneg"), ("1111nnnn01101101", "fsqrt"), ("1111nnnn00111101", "ftrc"),
    ("1111nnnnmmmm0000", "fadd"), ("1111nnnnmmmm0001", "fsub"), ("1111nnnnmmmm0010", "fmul"),
    ("1111nnnnmmmm0011", "fdiv"), ("1111nnnnmmmm0100", "fcmpeq"), ("1111nnnnmmmm0101", "fcmpgt"),
    ("1111nnnnmmmm0110", "fmovs_ld_r0x"), ("1111nnnnmmmm0111", "fmovs_st_r0x"),
    ("1111nnnnmmmm1000", "fmovs_ld"), ("1111nnnnmmmm1001", "fmovs_inc"), ("1111nnnnmmmm1010", "fmovs_st"),
    ("1111nnnnmmmm1011", "fmovs_dec"), ("1111nnnnmmmm1100", "fmov"), ("1111nnnnmmmm1110", "fmac"),
    ("0100nnnn01011010", "lds_fpul"), ("0100nnnn01010110", "ldsl_fpul"), ("0000nnnn01011010", "sts_fpul"),
    ("0100nnnn01010010", "stsl_fpul"), ("0100nnnn01101010", "lds_fpscr"), ("0100nnnn01100110", "ldsl_fpscr"),
    ("0000nnnn01101010", "sts_fpscr"), ("0100nnnn01100010", "stsl_fpscr"),
]
DELAYED = {"rts", "bts", "bfs", "bra", "bsr", "braf", "bsrf", "jmp", "jsr"}


def _compile(p):
    mask = val = 0
    for ch in p:
        mask <<= 1
        val <<= 1
        if ch in "01":
            mask |= 1
            val |= ch == "1"
    return mask, val


TABLE = [(_compile(p), name, p) for p, name in PATTERNS]


def field(op, p, ch):
    v = 0
    for i, c in enumerate(p):
        if c == ch:
            v = (v << 1) | ((op >> (15 - i)) & 1)
    return v


def decode(op):
    for (mask, val), name, p in TABLE:
        if op & mask == val:
            return name, field(op, p, "n"), field(op, p, "m"), field(op, p, "i"), field(op, p, "d")
    return None, 0, 0, 0, 0


# ---- emission -------------------------------------------------------------------------
def R(i):
    return f"c->r[{i}]"


def emit_simple(name, n, m, i, d, addr):
    """C for one non-branch instruction at addr (statements, no trailing newline)."""
    rn, rm = R(n), R(m)
    pc4 = addr + 4
    if name == "nop": return ""
    if name == "clrt": return "c->T = 0;"
    if name == "sett": return "c->T = 1;"
    if name == "clrs": return "c->S = 0;"
    if name == "sets": return "c->S = 1;"
    if name == "clrmac": return "c->mach = c->macl = 0;"
    if name == "div0u": return "c->Q = c->M = c->T = 0;"
    if name == "mov": return f"{rn} = {rm};"
    if name == "mov_imm": return f"{rn} = (u32)(s32){s8(i)};"
    if name == "movw_pc":
        a = pc4 + d * 2
        if ro(a):
            v = struct.unpack_from("<h", IMG, a - BASE)[0]
            return f"{rn} = (u32)(s32){v};  /* @{a:08x} */"
        return f"{rn} = (u32)(s32)(s16)rd16(0x{a:08x});"
    if name == "movl_pc":
        a = (pc4 & ~3) + d * 4
        if ro(a):
            return f"{rn} = 0x{u32(a):08x}u;  /* @{a:08x} */"
        return f"{rn} = rd32(0x{a:08x});"
    if name == "mova": return f"{R(0)} = 0x{(pc4 & ~3) + d * 4:08x}u;"
    st = {"movb_st": ("wr8", "(u8)"), "movw_st": ("wr16", "(u16)"), "movl_st": ("wr32", "")}
    if name in st:
        fn, cast = st[name]
        return f"{fn}({rn}, {cast}{rm});"
    ld = {"movb_ld": "(u32)(s32)(s8)rd8", "movw_ld": "(u32)(s32)(s16)rd16", "movl_ld": "rd32"}
    if name in ld: return f"{rn} = {ld[name]}({rm});"
    dec = {"movb_dec": (1, "wr8", "(u8)"), "movw_dec": (2, "wr16", "(u16)"), "movl_dec": (4, "wr32", "")}
    if name in dec:
        sz, fn, cast = dec[name]
        return f"{{ u32 v = {rm}; {rn} -= {sz}; {fn}({rn}, {cast}v); }}"
    inc = {"movb_inc": (1, "(u32)(s32)(s8)rd8"), "movw_inc": (2, "(u32)(s32)(s16)rd16"), "movl_inc": (4, "rd32")}
    if name in inc:
        sz, fn = inc[name]
        upd = "" if n == m else f" {rm} += {sz};"
        return f"{{ u32 v = {fn}({rm});{upd} {rn} = v; }}"
    if name == "movb_st_r0d": return f"wr8({R(n)} + {d}, (u8){R(0)});"
    if name == "movw_st_r0d": return f"wr16({R(n)} + {d * 2}, (u16){R(0)});"
    if name == "movl_st_d": return f"wr32({rn} + {d * 4}, {rm});"
    if name == "movb_ld_r0d": return f"{R(0)} = (u32)(s32)(s8)rd8({rm} + {d});"
    if name == "movw_ld_r0d": return f"{R(0)} = (u32)(s32)(s16)rd16({rm} + {d * 2});"
    if name == "movl_ld_d": return f"{rn} = rd32({rm} + {d * 4});"
    if name == "movb_st_r0x": return f"wr8({rn} + {R(0)}, (u8){rm});"
    if name == "movw_st_r0x": return f"wr16({rn} + {R(0)}, (u16){rm});"
    if name == "movl_st_r0x": return f"wr32({rn} + {R(0)}, {rm});"
    if name == "movb_ld_r0x": return f"{rn} = (u32)(s32)(s8)rd8({rm} + {R(0)});"
    if name == "movw_ld_r0x": return f"{rn} = (u32)(s32)(s16)rd16({rm} + {R(0)});"
    if name == "movl_ld_r0x": return f"{rn} = rd32({rm} + {R(0)});"
    if name == "movb_st_gbr": return f"wr8(c->gbr + {d}, (u8){R(0)});"
    if name == "movw_st_gbr": return f"wr16(c->gbr + {d * 2}, (u16){R(0)});"
    if name == "movl_st_gbr": return f"wr32(c->gbr + {d * 4}, {R(0)});"
    if name == "movb_ld_gbr": return f"{R(0)} = (u32)(s32)(s8)rd8(c->gbr + {d});"
    if name == "movw_ld_gbr": return f"{R(0)} = (u32)(s32)(s16)rd16(c->gbr + {d * 2});"
    if name == "movl_ld_gbr": return f"{R(0)} = rd32(c->gbr + {d * 4});"
    if name == "movt": return f"{rn} = c->T;"
    if name == "add": return f"{rn} += {rm};"
    if name == "add_imm": return f"{rn} += (u32)(s32){s8(i)};"
    if name == "addc":
        return (f"{{ u32 t1 = {rn} + {rm}, t0 = {rn}; {rn} = t1 + c->T; c->T = t0 > t1; if (t1 > {rn}) c->T = 1; }}")
    if name == "sub": return f"{rn} -= {rm};"
    if name == "subc":
        return (f"{{ u32 t1 = {rn} - {rm}, t0 = {rn}; {rn} = t1 - c->T; c->T = t0 < t1; if (t1 < {rn}) c->T = 1; }}")
    if name == "neg": return f"{rn} = 0u - {rm};"
    if name == "negc":
        return f"{{ u32 t = 0u - {rm}; {rn} = t - c->T; c->T = (0 < t) || (t < {rn}); }}"
    if name == "and": return f"{rn} &= {rm};"
    if name == "and_imm": return f"{R(0)} &= 0x{i:02x}u;"
    if name == "or": return f"{rn} |= {rm};"
    if name == "or_imm": return f"{R(0)} |= 0x{i:02x}u;"
    if name == "xor": return f"{rn} ^= {rm};"
    if name == "xor_imm": return f"{R(0)} ^= 0x{i:02x}u;"
    if name == "not": return f"{rn} = ~{rm};"
    if name == "tst": return f"c->T = ({rn} & {rm}) == 0;"
    if name == "tst_imm": return f"c->T = ({R(0)} & 0x{i:02x}u) == 0;"
    if name == "cmpeq_imm": return f"c->T = {R(0)} == (u32)(s32){s8(i)};"
    if name == "cmpeq": return f"c->T = {rn} == {rm};"
    if name == "cmphs": return f"c->T = {rn} >= {rm};"
    if name == "cmpge": return f"c->T = (s32){rn} >= (s32){rm};"
    if name == "cmphi": return f"c->T = {rn} > {rm};"
    if name == "cmpgt": return f"c->T = (s32){rn} > (s32){rm};"
    if name == "cmppl": return f"c->T = (s32){rn} > 0;"
    if name == "cmppz": return f"c->T = (s32){rn} >= 0;"
    if name == "cmpstr":
        return (f"{{ u32 t = {rn} ^ {rm}; c->T = !(t & 0xff000000u) || !(t & 0xff0000u) || "
                f"!(t & 0xff00u) || !(t & 0xffu); }}")
    if name == "div0s": return f"c->Q = {rn} >> 31; c->M = {rm} >> 31; c->T = c->M ^ c->Q;"
    if name == "div1": return f"sh4_div1(c, {n}, {m});"
    if name == "extub": return f"{rn} = (u8){rm};"
    if name == "extuw": return f"{rn} = (u16){rm};"
    if name == "extsb": return f"{rn} = (u32)(s32)(s8){rm};"
    if name == "extsw": return f"{rn} = (u32)(s32)(s16){rm};"
    if name == "swapb": return f"{{ u32 v = {rm}; {rn} = (v & 0xffff0000u) | ((v & 0xff) << 8) | ((v >> 8) & 0xff); }}"
    if name == "swapw": return f"{{ u32 v = {rm}; {rn} = (v << 16) | (v >> 16); }}"
    if name == "xtrct": return f"{rn} = ({rn} >> 16) | ({rm} << 16);"
    if name in ("shll", "shal"): return f"c->T = {rn} >> 31; {rn} <<= 1;"
    if name == "shlr": return f"c->T = {rn} & 1; {rn} >>= 1;"
    if name == "shar": return f"c->T = {rn} & 1; {rn} = (u32)((s32){rn} >> 1);"
    if name == "shll2": return f"{rn} <<= 2;"
    if name == "shlr2": return f"{rn} >>= 2;"
    if name == "shll8": return f"{rn} <<= 8;"
    if name == "shlr8": return f"{rn} >>= 8;"
    if name == "shll16": return f"{rn} <<= 16;"
    if name == "shlr16": return f"{rn} >>= 16;"
    if name == "shad":
        return (f"{{ u32 s = {rm}; if (!(s & 0x80000000u)) {rn} <<= s & 31; else if (!(s & 31)) "
                f"{rn} = (u32)((s32){rn} >> 31); else {rn} = (u32)((s32){rn} >> ((~s & 31) + 1)); }}")
    if name == "shld":
        return (f"{{ u32 s = {rm}; if (!(s & 0x80000000u)) {rn} <<= s & 31; else if (!(s & 31)) "
                f"{rn} = 0; else {rn} >>= (~s & 31) + 1; }}")
    if name == "rotl": return f"c->T = {rn} >> 31; {rn} = ({rn} << 1) | c->T;"
    if name == "rotr": return f"c->T = {rn} & 1; {rn} = ({rn} >> 1) | (c->T << 31);"
    if name == "rotcl": return f"{{ u32 t = c->T; c->T = {rn} >> 31; {rn} = ({rn} << 1) | t; }}"
    if name == "rotcr": return f"{{ u32 t = c->T; c->T = {rn} & 1; {rn} = ({rn} >> 1) | (t << 31); }}"
    if name == "dt": return f"{rn} -= 1; c->T = {rn} == 0;"
    if name == "mull": return f"c->macl = {rn} * {rm};"
    if name == "mulsw": return f"c->macl = (u32)((s32)(s16){rn} * (s32)(s16){rm});"
    if name == "muluw": return f"c->macl = (u32)((u16){rn} * (u16){rm});"
    if name == "dmulsl":
        return f"{{ s64 v = (s64)(s32){rn} * (s64)(s32){rm}; c->mach = (u32)((u64)v >> 32); c->macl = (u32)v; }}"
    if name == "dmulul":
        return f"{{ u64 v = (u64){rn} * (u64){rm}; c->mach = (u32)(v >> 32); c->macl = (u32)v; }}"
    if name == "sts_pr": return f"{rn} = c->pr;"
    if name == "stsl_pr": return f"{rn} -= 4; wr32({rn}, c->pr);"
    if name == "lds_pr": return f"c->pr = {rn};"
    if name == "ldsl_pr": return f"c->pr = rd32({rn}); {rn} += 4;"
    if name == "sts_mach": return f"{rn} = c->mach;"
    if name == "sts_macl": return f"{rn} = c->macl;"
    if name == "lds_mach": return f"c->mach = {rn};"
    if name == "lds_macl": return f"c->macl = {rn};"
    if name == "stsl_macl": return f"{rn} -= 4; wr32({rn}, c->macl);"
    if name == "ldsl_macl": return f"c->macl = rd32({rn}); {rn} += 4;"
    if name == "stsl_mach": return f"{rn} -= 4; wr32({rn}, c->mach);"
    if name == "ldsl_mach": return f"c->mach = rd32({rn}); {rn} += 4;"
    if name == "stc_gbr": return f"{rn} = c->gbr;"
    if name == "ldc_gbr": return f"c->gbr = {rn};"
    if name == "pref": return f"sh4_pref(c, {rn});"
    if name == "macl_": return f"sh4_macl(c, {n}, {m});"
    if name == "macw": return f"sh4_macw(c, {n}, {m});"
    if name == "tasb": return f"{{ u8 v = rd8({rn}); c->T = v == 0; wr8({rn}, v | 0x80); }}"
    if name == "movcal": return f"wr32({rn}, {R(0)});"
    if name in ("ocbi", "ocbp", "ocbwb"): return ""   # operand cache control: no effect on the host
    if name == "addv":
        return f"{{ s64 v = (s64)(s32){rn} + (s64)(s32){rm}; {rn} = (u32)v; c->T = v != (s32)v; }}"
    if name == "subv":
        return f"{{ s64 v = (s64)(s32){rn} - (s64)(s32){rm}; {rn} = (u32)v; c->T = v != (s32)v; }}"
    gb = {"andb_gbr": "&", "orb_gbr": "|", "xorb_gbr": "^"}
    if name in gb:
        return f"{{ u32 a = c->gbr + {R(0)}; wr8(a, rd8(a) {gb[name]} 0x{i:02x}); }}"
    if name == "tstb_gbr": return f"c->T = (rd8(c->gbr + {R(0)}) & 0x{i:02x}) == 0;"
    if name == "stc_sr": return f"{rn} = sh4_get_sr(c);"
    if name == "ldc_sr": return f"sh4_set_sr(c, {rn});"
    if name == "stcl_sr": return f"{rn} -= 4; wr32({rn}, sh4_get_sr(c));"
    if name == "ldcl_sr": return f"{{ u32 v = rd32({rn}); {rn} += 4; sh4_set_sr(c, v); }}"
    if name == "stcl_gbr": return f"{rn} -= 4; wr32({rn}, c->gbr);"
    if name == "ldcl_gbr": return f"c->gbr = rd32({rn}); {rn} += 4;"
    if name == "stc_vbr": return f"{rn} = c->vbr;"
    if name == "ldc_vbr": return f"c->vbr = {rn};"
    if name == "stcl_vbr": return f"{rn} -= 4; wr32({rn}, c->vbr);"
    if name == "ldcl_vbr": return f"c->vbr = rd32({rn}); {rn} += 4;"
    return emit_fpu(name, n, m, addr)


def emit_fpu(name, n, m, addr):
    rn, rm = R(n), R(m)
    op = u16(addr)
    g = (op >> 8) & 0xF   # Flycast GetN(op): full 4-bit field
    if name in ("fadd", "fsub", "fmul", "fdiv", "fcmpeq", "fcmpgt", "fmov", "fmac"):
        return f"sh4_{name}(c, {n}, {m});"
    if name == "fmovs_ld_r0x": return f"sh4_fmov_ld(c, {n}, {rm} + c->r[0]);"
    if name == "fmovs_st_r0x": return f"sh4_fmov_st(c, {m}, {rn} + c->r[0]);"
    if name == "fmovs_ld": return f"sh4_fmov_ld(c, {n}, {rm});"
    if name == "fmovs_inc": return f"{{ u32 a = {rm}; sh4_fmov_ld(c, {n}, a); {rm} = a + sh4_fsz(c); }}"
    if name == "fmovs_st": return f"sh4_fmov_st(c, {m}, {rn});"
    if name == "fmovs_dec": return f"{{ u32 a = {rn} - sh4_fsz(c); sh4_fmov_st(c, {m}, a); {rn} = a; }}"
    if name in ("fabs", "fsrra", "fcnvds", "fcnvsd", "fldi0", "fldi1", "float", "fneg", "fsqrt", "ftrc"):
        return f"sh4_{name}(c, {g});"
    if name in ("fipr", "ftrv", "fsca"): return f"sh4_{name}(c, {g});"
    if name == "flds": return f"c->fpul = frbits(c, {g});"
    if name == "fsts": return f"set_frbits(c, {g}, c->fpul);"
    if name == "frchg": return "sh4_frchg(c);"
    if name == "fschg": return "sh4_fschg(c);"
    if name == "lds_fpul": return f"c->fpul = {rn};"
    if name == "ldsl_fpul": return f"c->fpul = rd32({rn}); {rn} += 4;"
    if name == "sts_fpul": return f"{rn} = c->fpul;"
    if name == "stsl_fpul": return f"{rn} -= 4; wr32({rn}, c->fpul);"
    if name == "lds_fpscr": return f"sh4_set_fpscr(c, {rn});"
    if name == "ldsl_fpscr": return f"{{ u32 v = rd32({rn}); {rn} += 4; sh4_set_fpscr(c, v); }}"
    if name == "sts_fpscr": return f"{rn} = c->fpscr;"
    if name == "stsl_fpscr": return f"{rn} -= 4; wr32({rn}, c->fpscr);"
    return None


# Context-save routines with setjmp semantics (return 0, later "return again" with another
# value when a restore reloads the context). A call to one is emitted as a host setjmp in the
# caller's own frame, keyed by the buffer in r4, so the restore can longjmp back into it.
SETJMP_FUNCS = {0x8C102E84}

# Runtime hooks: `sh4_hook(c, addr)` is emitted before the instruction at each address listed in
# sh4c/hooks.txt (hex, one per line, # comments): the native runtime's save / load / rollback
# point is the top of main's loop.
def _load_hooks():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hooks.txt")
    if not os.path.exists(p):
        return set()
    return {int(l.split("#")[0], 16) for l in open(p) if l.split("#")[0].strip()}
HOOKS = _load_hooks()


def loop_jump(t, a, inside):
    """goto for a branch at a to t. A tight backward branch (a loop of at most 32 instructions
    with no call in it, like the sound library's delay loops) first charges its length in
    instructions to the virtual clock (sh4_loop_cycles, drained by the runtime), so a busy-wait
    takes time as on hardware. Every 4096 such instructions it calls sh4_loop_poll, which runs
    the clock and takes pending interrupts as a call does: a loop that waits for a flag an
    interrupt handler or another thread sets (the KO slow motion's wait, 8C024F6A) would
    otherwise spin forever. Other loops are timed by their calls, as before."""
    if t not in inside:
        return f"{{ sh4_call(c, 0x{t:08x}u); return; }}"
    if t <= a and (a - t) // 2 + 2 <= 32:
        calls = ("jsr", "bsr", "bsrf", "jmp", "braf", "trapa")
        if not any(decode(u16(x))[0] in calls for x in range(t, a + 4, 2)):
            return f"{{ sh4_loop_cycles += {(a - t) // 2 + 2}u; if (sh4_loop_cycles >= 4096u) sh4_loop_poll(c); goto L_{t:08x}; }}"
    return f"goto L_{t:08x};"


def call_stmt(target_expr, a, dcode, static_target=None):
    """C for a call (jsr/bsr/bsrf) at a: PR, delay slot, then the call."""
    if static_target in SETJMP_FUNCS:
        return (f"{{ u32 t = {target_expr}; c->pr = 0x{a + 4:08x}u; {dcode} "
                f"if (!sh4_setjmp(*sh4_ctx_slot(c, c->r[4]))) sh4_call(c, t); else sh4_ctx_resumed(c); }}")
    return f"{{ u32 t = {target_expr}; c->pr = 0x{a + 4:08x}u; {dcode} sh4_call(c, t); }}"


# Function entries from functions.json. Flow recovery stops at them (a branch or fallthrough
# into another entry becomes a tail call). Filled by main() / sweep.py.
ENTRIES = set()


class _ReadOnly:
    """`in` test for "any address in the read-only code blocks"."""
    def __contains__(self, a):
        return ro(a)


def recover(entry, ranges):
    """Function body = Ghidra's ranges plus everything reachable from the entry by control flow
    (fallthrough, branches, jump tables). Ghidra misses case bodies that are only reached
    through `braf` tables. Returns (sorted body addresses, reachable addresses)."""
    body = {a for lo, hi in ranges for a in range(lo, hi + 1, 2)}
    walked, reach, work = set(), set(), [entry]
    while work:
        a = work.pop()
        while a not in walked:
            if (a != entry and a in ENTRIES) or not (ro(a) or a in body):   # code outside the
                # read-only blocks (a few routines near the end of the image) is taken from Ghidra
                break
            walked.add(a)
            reach.add(a)
            name, n, m, i, d = decode(u16(a))
            if name is None:
                break
            if name in ("bt", "bf", "bts", "bfs"):
                work.append(a + 4 + s8(d) * 2)
            elif name == "bra":
                work.append(a + 4 + s12(d) * 2)
            elif name == "braf":
                work.extend(jump_table(a, n, _ReadOnly()))
            elif name == "jmp":
                for t in jmp_targets(a, n)[1]:   # long branches and pointer-table switches
                    if t not in ENTRIES and ro(t) and not t & 1:
                        work.append(t)
            if name in DELAYED:
                reach.add(a + 2)
                if name in ("rts", "bra", "jmp", "braf"):
                    break
                a += 4
                continue
            a += 2
    return sorted(body | reach), reach


_NO_RN_WRITE = {"movb_st", "movw_st", "movl_st", "movl_st_d", "movb_st_r0x", "movw_st_r0x", "movl_st_r0x",
                "pref", "ocbi", "ocbp", "ocbwb", "movcal", "div0s", "mull", "mulsw", "muluw", "dmulsl",
                "dmulul", "jsr", "jmp", "braf", "bsrf", "tasb", "tst", "fmovs_st"}


def writes_rn(name):
    """Does this instruction modify the register in its n field?"""
    if name == "fmovs_dec":
        return True
    return not (name in _NO_RN_WRITE or name.startswith(("cmp", "f", "lds_", "ldc_")))


def jmp_targets(a, n):
    """Statically known targets of `jmp @Rn`, by looking at how Rn was set just before:
      mov.l @(disp,PC),Rn                       -> ("literal", [target])
      mov.l @(disp,PC),Rb / mov.l @(R0,Rb),Rn   -> ("table", [targets...])  (absolute pointers)
      anything else                             -> ("register", [])
    Only guesses: the emitted code compares them with the real Rn before taking a goto."""
    for b in range(a - 2, a - 40, -2):
        name, bn, bm, bi, bd = decode(u16(b))
        if name in ("rts", "bra", "jmp", "braf") or name is None:
            break
        if bn != n or not writes_rn(name):
            continue
        if name == "movl_pc":
            lit = ((b + 4) & ~3) + bd * 4
            return ("literal", [u32(lit)]) if ro(lit) else ("register", [])
        if name == "movl_ld_r0x":
            for c in range(b - 2, b - 40, -2):
                cn, cnn, cm, ci, cd = decode(u16(c))
                if cn in ("rts", "bra", "jmp", "braf") or cn is None:
                    break
                if cn == "movl_pc" and cnn == bm:
                    base = u32(((c + 4) & ~3) + cd * 4)
                    out = []
                    for k in range(64):
                        if not ro(base + 4 * k):
                            break
                        t = u32(base + 4 * k)
                        if not ro(t) or t & 1 or abs(t - a) > 0x1000:
                            break
                        out.append(t)
                    return ("table", out)
                if cnn == bm and writes_rn(cn):
                    break
        return ("register", [])
    return ("register", [])


def jmp_literal(a, n):
    kind, ts = jmp_targets(a, n)
    return ts[0] if kind == "literal" else None


def jump_table(a, n, inside):
    """Case targets of a `braf Rn` switch: mova table,r0 / mov.w @(r0,Ri),Rn / braf Rn.
    Entries are s16 offsets from a+4. The count comes from the bounds check (mov #k,Rl /
    cmp/hs Rl,Ri) when found, else entries are read while they land inside the function."""
    base = count = lim = None
    for b in range(a - 2, a - 40, -2):
        if b not in inside:
            break
        name, bn, bm, bi, bd = decode(u16(b))
        if name in ("rts", "jmp"):
            break
        if name == "mova" and base is None:
            base = ((b + 4) & ~3) + bd * 4
        elif name in ("cmphs", "cmphi") and lim is None:
            lim = (bm, name)
        elif name == "mov_imm" and lim and bn == lim[0] and count is None:
            count = s8(bi) + (1 if lim[1] == "cmphi" else 0)
    if base is None:
        return []
    out = []
    for k in range(count if count and 0 < count <= 256 else 256):
        if not ro(base + 2 * k):   # the table itself is data, outside Ghidra's body ranges
            break
        t = a + 4 + struct.unpack_from("<h", IMG, base + 2 * k - BASE)[0]
        if t not in inside or t & 1 or base <= t < base + 2 * (k + 1):
            break
        out.append(t)
    return out


def translate(entry, ranges):
    addrs, _ = recover(entry, ranges)
    inside = set(addrs)
    insns = {a: (u16(a),) + decode(u16(a)) for a in addrs}
    # branch targets inside the function become labels
    labels = set()
    tables = {}
    for a, (op, name, n, m, i, d) in insns.items():
        if name in ("bt", "bf", "bts", "bfs"):
            t = a + 4 + s8(d) * 2
        elif name in ("bra", "bsr"):
            t = a + 4 + s12(d) * 2
        elif name == "braf":
            tables[a] = jump_table(a, n, inside)
            labels.update(tables[a])
            continue
        elif name == "jmp":
            tables[a] = [t for t in jmp_targets(a, n)[1] if t in inside and t != entry]
            labels.update(tables[a])
            continue
        else:
            continue
        if t in inside:
            labels.add(t)
    out = [f"void f_{entry:08x}(SH4 *c)", "{"]
    if entry != addrs[0]:   # entry is not the lowest address of the body
        labels.add(entry)
        out.append(f"\tgoto L_{entry:08x};")
    skip = set()
    expect = None   # address execution continues at after the last emitted instruction
    for a in addrs:
        if a in skip:
            continue
        if expect is not None and expect != a:
            out.append(f"\t{{ sh4_call(c, 0x{expect:08x}u); return; }}  /* falls through to {expect:08x} */")
        op, name, n, m, i, d = insns[a]
        expect = a + 2
        if a in labels:
            out.append(f"L_{a:08x}:;")
        if a in HOOKS:
            out.append(f"\tsh4_hook(c, 0x{a:08x}u);")
        if name is None:
            out.append(f"\tsh4_unimpl(c, 0x{a:08x}u, 0x{op:04x}); return;")
            expect = None
            continue
        if name in DELAYED:
            # terminal branches end the flow; the others continue after the delay slot unless
            # the delay slot is a label (then a goto to a+4 is emitted)
            expect = None if name in ("rts", "bra", "jmp", "braf") or a + 2 in labels else a + 4
        if name in ("bt", "bf"):
            t = a + 4 + s8(d) * 2
            cond = "c->T" if name == "bt" else "!c->T"
            jump = loop_jump(t, a, inside)
            out.append(f"\tif ({cond}) {jump}  /* {a:08x} {name} */")
            continue
        if name in DELAYED:
            ds = a + 2
            dop, dname, dn, dm, di, dd = (u16(ds),) + decode(u16(ds))
            dcode = emit_simple(dname, dn, dm, di, dd, ds) if dname and dname not in DELAYED and dname not in ("bt", "bf") else None
            if dcode is None:
                dcode = f"sh4_unimpl(c, 0x{ds:08x}u, 0x{dop:04x});"
            body = []
            if name in ("bts", "bfs"):
                t = a + 4 + s8(d) * 2
                cond = "c->T" if name == "bts" else "!c->T"
                jump = loop_jump(t, a, inside)
                body.append(f"\t{{ int cond = {cond}; {dcode} if (cond) {jump} }}  /* {a:08x} {name} */")
                if ds in labels:
                    body.append(f"\tgoto L_{a + 4:08x};")
                    labels.add(a + 4)
                else:
                    skip.add(ds)
            elif name == "rts":
                body.append(f"\t{{ {dcode} return; }}  /* {a:08x} rts */")
                if ds not in labels:
                    skip.add(ds)
            elif name in ("bra", "bsr"):
                t = a + 4 + s12(d) * 2
                if name == "bsr":
                    body.append(f"\t{call_stmt(f'0x{t:08x}u', a, dcode, t)}  /* {a:08x} bsr */")
                    if ds in labels:
                        body.append(f"\tgoto L_{a + 4:08x};")
                        labels.add(a + 4)
                    else:
                        skip.add(ds)
                else:
                    jump = loop_jump(t, a, inside)
                    body.append(f"\t{{ {dcode} {jump} }}  /* {a:08x} bra */")
                    if ds not in labels:
                        skip.add(ds)
            elif name in ("jsr", "bsrf"):
                tgt = R(n) if name == "jsr" else f"(0x{a + 4:08x}u + {R(n)})"
                lit = jmp_targets(a, n) if name == "jsr" else ("register", [])
                body.append(f"\t{call_stmt(tgt, a, dcode, lit[1][0] if lit[0] == 'literal' else None)}  /* {a:08x} {name} */")
                if ds in labels:
                    body.append(f"\tgoto L_{a + 4:08x};")
                    labels.add(a + 4)
                else:
                    skip.add(ds)
            elif name in ("jmp", "braf"):
                tgt = R(n) if name == "jmp" else f"(0x{a + 4:08x}u + {R(n)})"
                cases = "".join(f" case 0x{t:08x}u: goto L_{t:08x};" for t in sorted(set(tables.get(a, []))))
                sw = f" switch (t) {{{cases} }}" if cases else ""
                body.append(f"\t{{ u32 t = {tgt}; {dcode}{sw} sh4_jump(c, t); return; }}  /* {a:08x} {name} */")
                if ds not in labels:
                    skip.add(ds)
            out.extend(body)
            continue
        code = emit_simple(name, n, m, i, d, a)
        if code is None:
            out.append(f"\tsh4_unimpl(c, 0x{a:08x}u, 0x{op:04x}); return;  /* {name} */")
            expect = None
        elif code:
            out.append(f"\t{code}  /* {a:08x} {name} */")
    # labels referenced after the loop (fallthrough targets) must exist
    emitted = {line[2:10] for line in out if line.startswith("L_")}
    missing = [t for t in labels if f"{t:08x}" not in emitted]
    if expect is not None:
        out.append(f"\t{{ sh4_call(c, 0x{expect:08x}u); return; }}  /* falls through to {expect:08x} */")
    out.append("\treturn;")
    for t in sorted(missing):
        out.append(f"L_{t:08x}: sh4_unimpl(c, 0x{t:08x}u, 0); return;")
    out.append("}")
    return "\n".join(out)


def main():
    outp = sys.argv[1]
    funcs = {int(f["entry"], 16): f for f in json.load(open(os.path.join(SC, "port", "functions.json")))}
    ENTRIES.update(funcs)
    parts = ['#include "sh4rt.h"', ""]
    for e in sys.argv[2:]:
        entry = int(e, 16)
        f = funcs[entry]
        ranges = [(int(lo, 16), int(hi, 16)) for lo, hi in f["ranges"]]
        parts.append(f"/* {f['name']} */")
        parts.append(translate(entry, ranges))
        parts.append("")
    open(outp, "w").write("\n".join(parts))
    print("wrote", outp)


if __name__ == "__main__":
    main()
