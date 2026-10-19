"""Cluster the memtrace write set into regions and attribute writers.

usage: python -I memregions.py [traces_dir] [gap_bytes]
Prints one line per region: range, size, bytes actually written, max/median frames written
(out of the capture window), and the top writer functions (by first/last writer PC).
"""
import bisect
import collections
import csv
import os
import statistics
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def load_funcs():
    rows = []
    with open(os.path.join(SC, "facts", "functions.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            rows.append((int(r["entry"], 16), int(r["size"]), r["name"]))
    rows.sort()
    return rows, [r[0] for r in rows]


def main():
    tdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SC, "traces")
    gap = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    rows, starts = load_funcs()

    def fname(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and rows[i][0] <= pc < rows[i][0] + rows[i][1]:
            return rows[i][2] if not rows[i][2].startswith("FUN_") else f"{rows[i][0]:08x}"
        return f"?{pc:08x}"

    lines = []
    with open(os.path.join(tdir, "mem_writes.csv"), encoding="utf-8") as f:
        for r in csv.DictReader(f):
            lines.append((int(r["line"], 16), bin(int(r["mask"], 16)).count("1"), int(r["frames"]),
                          int(r["first_pc"], 16), int(r["last_pc"], 16)))
    lines.sort()
    regions = []
    cur = [lines[0]]
    for ln in lines[1:]:
        if ln[0] - (cur[-1][0] + 32) <= gap:
            cur.append(ln)
        else:
            regions.append(cur)
            cur = [ln]
    regions.append(cur)
    total = 0
    print(f"{'range':21s} {'span':>8s} {'written':>8s} {'fmax':>5s} {'fmed':>5s}  writers")
    for reg in regions:
        lo, hi = reg[0][0], reg[-1][0] + 32
        written = sum(x[1] for x in reg)
        total += written
        fr = [x[2] for x in reg]
        writers = collections.Counter()
        for x in reg:
            writers[fname(x[3])] += 1
            writers[fname(x[4])] += 1
        top = ", ".join(f"{n}({c})" for n, c in writers.most_common(4))
        print(f"{lo:08x}-{hi:08x} {hi - lo:8d} {written:8d} {max(fr):5d} {int(statistics.median(fr)):5d}  {top}")
    print(f"# {len(regions)} regions, {total} bytes written in total")


if __name__ == "__main__":
    main()
