"""Find functions that load screen-size constants from literal pools.

usage: python -I screen_consts.py [facts_dir]
Prints one line per function with the constants it loads, grouped by address range.
"""
import collections
import csv
import os
import sys

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "facts")

CONSTS = {
    0x44200000: "f640", 0x441FC000: "f639", 0x43F00000: "f480", 0x43EF8000: "f479",
    0x43A00000: "f320", 0x43700000: "f240", 0x43200000: "f160", 0x42F00000: "f120",
    0x3F2AAAAB: "f0.6667", 0x3FAAAAAB: "f1.3333", 0x3F400000: "f0.75",
    0x3B4CCCCD: "f1/320", 0x3B888889: "f1/240", 0x3ACCCCCD: "f1/640", 0x3B088889: "f1/480",
    0x00000280: "i640", 0x0000027F: "i639", 0x000001E0: "i480", 0x000001DF: "i479",
    0x00000140: "i320", 0x000000F0: "i240",
    0x01DF027F: "FB_CLIP_639_479", 0x027F0000: "FB_X_CLIP", 0x01DF0000: "FB_Y_CLIP",
}


def main():
    names = {}
    with open(os.path.join(D, "functions.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            names[r["entry"]] = r["name"]
    hits = collections.defaultdict(collections.Counter)
    with open(os.path.join(D, "literals.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
            v = int(r["value"], 16)
            if v in CONSTS:
                hits[r["func"]][CONSTS[v]] += 1
    for e in sorted(hits, key=lambda x: int(x, 16)):
        c = hits[e]
        tags = " ".join(f"{k}x{n}" if n > 1 else k for k, n in sorted(c.items()))
        print(f"{e} {names.get(e, '?'):28s} {tags}")
    print(f"# {len(hits)} functions", file=sys.stderr)


if __name__ == "__main__":
    main()
