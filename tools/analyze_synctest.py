"""Summarise a SyncTest log: pass rate and which addresses diverge, mapped to save-state
ranges, known globals and the functions that write them (from the memtrace).

usage: python -I analyze_synctest.py [synctest.csv]
"""
import bisect
import collections
import csv
import os
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def load_writers():
    w = {}
    p = os.path.join(SC, "traces", "mem_writes.csv")
    if os.path.exists(p):
        for r in csv.DictReader(open(p, encoding="utf-8")):
            w[int(r["line"], 16)] = (int(r["first_pc"], 16), int(r["last_pc"], 16))
    return w


def load_funcs():
    rows = []
    for r in csv.DictReader(open(os.path.join(SC, "facts", "functions.tsv"), encoding="utf-8"), delimiter="\t"):
        rows.append((int(r["entry"], 16), int(r["size"]), r["name"]))
    rows.sort()
    return rows, [r[0] for r in rows]


def load_labels():
    labels = []
    for fn in os.listdir(os.path.join(SC, "names")):
        for line in open(os.path.join(SC, "names", fn), encoding="utf-8"):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2 and p[1].startswith("g_"):
                labels.append((int(p[0], 16), p[1]))
    labels.sort()
    return labels


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SC, "traces", "synctest.csv")
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    ok = sum(r["result"] == "OK" for r in rows)
    print(f"{path}: {len(rows)} frames tested, {ok} OK, {len(rows) - ok} mismatched")
    if not rows or ok == len(rows):
        return
    writers = load_writers()
    funcs, starts = load_funcs()
    labels = load_labels()
    lstarts = [a for a, _ in labels]

    def fname(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and funcs[i][0] <= pc < funcs[i][0] + funcs[i][1]:
            return funcs[i][2]
        return f"?{pc:08x}"

    def label(addr):
        i = bisect.bisect_right(lstarts, addr) - 1
        if i >= 0 and addr - labels[i][0] < 0x100:
            return f"{labels[i][1]}+0x{addr - labels[i][0]:x}"
        return ""

    lines = collections.Counter()
    first_bad = None
    for r in rows:
        if r["result"] == "OK":
            continue
        first_bad = first_bad or r
        for d in filter(None, r["first_diffs"].split(";")):
            addr = int(d.split(":")[0], 16)
            lines[addr & ~31] += 1
    print(f"first mismatch: test {first_bad['test']} (battle frame {first_bad['state_frames']}), "
          f"{first_bad['diff_lines']} lines differ")
    print("most frequently diverging 32-byte lines (first 24 per frame are logged):")
    for line, n in lines.most_common(30):
        wp = writers.get(line)
        who = f"written by {fname(wp[0])} / {fname(wp[1])}" if wp else "not in memtrace"
        print(f"  {line:08x} x{n:4d}  {label(line):28s} {who}")


if __name__ == "__main__":
    main()
