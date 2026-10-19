"""Summarise sc/traces/hud_trace.csv: which game functions (and their callers) submitted
polygons during the captured battle frames, and where on screen those polygons landed.

usage: python -I analyze_trace.py [trace.csv]

PowerVR TA stream (32-byte blocks): a block whose word0 bits 31..29 are 4 or 5 is a polygon
or sprite parameter; 7 is a vertex (end-of-strip if bit 28 set). For the common vertex
formats word1/word2 are screen X/Y floats. Sprites (type 5 header) are followed by vertex
blocks with 4 corners packed as floats starting at word1.
"""
import bisect
import collections
import csv
import os
import struct
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def load_funcs():
    rows = []
    with open(os.path.join(SC, "facts", "functions.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            rows.append((int(r["entry"], 16), int(r["size"]), r["name"]))
    rows.sort()
    return rows


def fn_of(rows, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and rows[i][0] <= addr < rows[i][0] + rows[i][1]:
        return f"{rows[i][2]}@{rows[i][0]:08x}"
    return f"?@{addr:08x}"


def f32(word):
    return struct.unpack("<f", struct.pack("<I", word))[0]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SC, "traces", "hud_trace.csv")
    rows = load_funcs()
    starts = [r[0] for r in rows]
    stats = collections.defaultdict(lambda: {"blocks": 0, "verts": 0, "xs": [], "ys": [], "frames": set()})
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            pc, pr = int(r["pc"], 16), int(r["pr"], 16)
            w = [int(r[f"w{i}"], 16) for i in range(8)]
            key = (fn_of(rows, starts, pc), fn_of(rows, starts, pr))
            s = stats[key]
            s["blocks"] += 1
            s["frames"].add(r["frame"])
            if (w[0] >> 29) == 7:  # vertex parameter
                x, y = f32(w[1]), f32(w[2])
                if -2000 < x < 2000 and -2000 < y < 2000:
                    s["verts"] += 1
                    s["xs"].append(x)
                    s["ys"].append(y)
    print(f"{'submitter (pc)':42s} {'caller (pr)':42s} blocks verts  x-range           y-range")
    for (sub, caller), s in sorted(stats.items(), key=lambda kv: -kv[1]["blocks"]):
        xr = f"{min(s['xs']):7.1f}..{max(s['xs']):7.1f}" if s["xs"] else "-"
        yr = f"{min(s['ys']):7.1f}..{max(s['ys']):7.1f}" if s["ys"] else "-"
        print(f"{sub:42s} {caller:42s} {s['blocks']:6d} {s['verts']:5d}  {xr:17s} {yr}")


if __name__ == "__main__":
    main()
