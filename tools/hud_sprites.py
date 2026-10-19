"""Decode PowerVR sprite packets from a trace (2D HUD) into per-sprite bounding boxes.

Sprite vertex block = 64 bytes = two consecutive SQ flushes from the same PC/PR stream:
  block1: PCW, AX, AY, AZ, BX, BY, BZ, CX
  block2: CY, CZ, DX, DY, pad, AUV, BUV, CUV
usage: python -I hud_sprites.py trace.csv [frame] [lo_pc hi_pc]
"""
import collections
import csv
import struct
import sys


def f32(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def main():
    path = sys.argv[1]
    frame = sys.argv[2] if len(sys.argv) > 2 else "120"
    lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x8C064000
    hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x8C0B6000
    rows = [r for r in csv.DictReader(open(path, encoding="utf-8")) if r["frame"] == frame]
    out = []
    i = 0
    tex = None
    while i < len(rows):
        r = rows[i]
        pc = int(r["pc"], 16)
        w = [int(r[f"w{k}"], 16) for k in range(8)]
        if not lo <= pc < hi:
            i += 1
            continue
        if (w[0] >> 29) == 5:  # sprite parameter header
            tex = w[3]
            i += 1
            continue
        if (w[0] >> 29) == 7 and i + 1 < len(rows):
            w2 = [int(rows[i + 1][f"w{k}"], 16) for k in range(8)]
            xs = [f32(w[1]), f32(w[4]), f32(w[7]), f32(w2[2])]
            ys = [f32(w[2]), f32(w[5]), f32(w2[0]), f32(w2[3])]
            out.append((r["pc"], r["pr"], min(xs), max(xs), min(ys), max(ys), tex))
            i += 2
            continue
        i += 1
    groups = collections.defaultdict(list)
    for pc, pr, x0, x1, y0, y1, t in out:
        groups[pr].append((x0, x1, y0, y1, t))
    for pr, boxes in sorted(groups.items()):
        print(f"caller pr={pr}  sprites={len(boxes)}")
        for x0, x1, y0, y1, t in boxes:
            print(f"    x {x0:6.1f}..{x1:6.1f}  y {y0:5.1f}..{y1:5.1f}  w={x1 - x0:5.1f}  tex={(t or 0):08x}")


if __name__ == "__main__":
    main()
