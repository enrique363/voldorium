"""Diff SyncTest snapshot dumps (sync_<n>_end1.bin vs _end2.bin, plus _snap.bin) word by word.

usage: python -I diff_sync_dump.py <n> [traces_dir [ranges.tsv]]   (ranges.tsv: the table the dump was made with)
Prints differing 32-bit words grouped into runs: address, snap/end1/end2 as hex and float.
"""
import os
import struct
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def ranges(table=None):
    """Ranges in dump order: include (compared) and restore (saved, not compared)."""
    out = []
    for line in open(table or os.path.join(SC, "ggpo", "savestate_ranges.tsv"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        a, b, d = line.split("\t")[:3]
        if d in ("include", "restore"):
            out.append((int(a, 16), int(b, 16), d == "include"))
    return out


def main():
    n = sys.argv[1]
    d = sys.argv[2] if len(sys.argv) > 2 else os.path.join(SC, "traces")
    snap = open(os.path.join(d, f"sync_{n}_snap.bin"), "rb").read()
    e1 = open(os.path.join(d, f"sync_{n}_end1.bin"), "rb").read()
    e2 = open(os.path.join(d, f"sync_{n}_end2.bin"), "rb").read()
    o = 0
    diffs = []
    for lo, hi, cmp in ranges(sys.argv[3] if len(sys.argv) > 3 else None):
        for a in range(lo, hi, 4):
            if cmp and e1[o:o + 4] != e2[o:o + 4]:
                diffs.append((a, o))
            o += 4
    print(f"frame {n}: {len(diffs)} differing words")
    prev = None
    shown = 0
    for a, o in diffs:
        s, x, y = (struct.unpack_from("<I", b, o)[0] for b in (snap, e1, e2))
        fx, fy = (struct.unpack_from("<f", b, o)[0] for b in (e1, e2))
        gap = "" if prev is not None and a - prev <= 16 else "\n"
        prev = a
        if shown < 120:
            rel = (fx - fy) / fx if fx not in (0.0,) and abs(fx) < 1e30 else 0
            print(f"{gap}  {a:08x} snap={s:08x} run1={x:08x} ({fx:.6g}) run2={y:08x} ({fy:.6g}) rel={rel:.2e}")
            shown += 1


if __name__ == "__main__":
    main()
