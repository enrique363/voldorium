"""Classify functions by the Dreamcast hardware registers / constants they load.

usage: python -I classify.py <facts_dir>
Writes <facts_dir>/hw_classes.tsv and prints a summary.
"""
import collections
import csv
import os
import sys

# (lo, hi, tag) on the 29-bit physical address (P1/P2/P4 mirrors folded)
REGIONS = [
    (0x005F6800, 0x005F6900, "SB_SYS"),      # system bus: DMA, interrupts
    (0x005F6900, 0x005F6A00, "SB_INT"),
    (0x005F6C00, 0x005F6D00, "MAPLE"),       # controllers / VMU
    (0x005F7000, 0x005F7100, "GDROM"),
    (0x005F7400, 0x005F7500, "G1"),
    (0x005F7800, 0x005F7900, "G2"),
    (0x005F7C00, 0x005F7D00, "PVR_DMA"),
    (0x005F8000, 0x005F9000, "PVR"),         # PowerVR2 core regs (viewport, fb, scaler)
    (0x005F9000, 0x005FA000, "PVR_PAL"),
    (0x00700000, 0x00710000, "AICA"),
    (0x00710000, 0x00710100, "RTC"),
    (0x00800000, 0x00A00000, "AICA_RAM"),
    (0x04000000, 0x05000000, "VRAM64"),
    (0x05000000, 0x06000000, "VRAM32"),
    (0x10000000, 0x10800000, "TA_FIFO"),     # polygon submission
    (0x10800000, 0x11000000, "TA_YUV"),
    (0x11000000, 0x12000000, "TA_TEX"),
]
SH4_P4 = [
    (0xE0000000, 0xE4000000, "SQ"),          # store queues
    (0xFF000000, 0xFF000100, "SH4_MMU/CCN"),
    (0xFF200000, 0xFF200100, "SH4_UBC"),
    (0xFF800000, 0xFF800100, "SH4_BSC"),
    (0xFFA00000, 0xFFA00100, "SH4_DMAC"),
    (0xFFC00000, 0xFFC00100, "SH4_CPG"),
    (0xFFD00000, 0xFFD00100, "SH4_INTC"),
    (0xFFD80000, 0xFFD80100, "SH4_TMU"),
    (0xFFE00000, 0xFFE80100, "SH4_SCI/SCIF"),
]


def tag(v):
    if v in (0xE0000000, 0xFF000000) or v & 0x3:
        return None  # bare masks / unaligned values are not register addresses
    for lo, hi, t in SH4_P4:
        if lo <= v < hi:
            return t
    if v >= 0xE0000000:
        return None
    region = v >> 29
    if region not in (4, 5):  # P1/P2 only; bare U0 values are usually masks or counts
        return None
    p = v & 0x1FFFFFFF
    if 0x0C000000 <= p < 0x10000000:
        return None  # main RAM: pointer, not MMIO
    for lo, hi, t in REGIONS:
        if lo <= p < hi:
            return t
    return None


def main():
    d = sys.argv[1]
    funcs = {}
    with open(os.path.join(d, "functions.tsv"), encoding="utf-8") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            funcs[row["entry"]] = row
    tags = collections.defaultdict(collections.Counter)
    regs = collections.defaultdict(set)
    with open(os.path.join(d, "literals.tsv"), encoding="utf-8") as f:
        for row in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
            v = int(row["value"], 16)
            t = tag(v)
            if t:
                tags[row["func"]][t] += 1
                regs[row["func"]].add(f"{v:08x}")
    with open(os.path.join(d, "hw_classes.tsv"), "w", encoding="utf-8") as o:
        o.write("entry\tsize\tncallers\ttags\tregs\n")
        for e in sorted(tags):
            fn = funcs.get(e, {})
            o.write(f"{e}\t{fn.get('size')}\t{fn.get('ncallers')}\t"
                    f"{','.join(f'{k}:{n}' for k, n in tags[e].most_common())}\t{' '.join(sorted(regs[e]))}\n")
    total = collections.Counter()
    for c in tags.values():
        for k in c:
            total[k] += 1
    print("functions touching each region:", dict(total.most_common()))
    print("hardware-touching functions:", len(tags), "of", len(funcs))
    addrs = sorted(int(e, 16) for e in tags)
    print("address span of hw functions: %08x - %08x" % (addrs[0], addrs[-1]))


if __name__ == "__main__":
    main()
