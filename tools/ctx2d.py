"""Context for 2D-drawing candidates: size, callers, strings used by the function and its callers.

usage: python -I ctx2d.py <screen_consts.txt>
"""
import collections
import csv
import os
import sys

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "facts")


def main():
    funcs, callers = {}, collections.defaultdict(list)
    with open(os.path.join(D, "functions.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            funcs[r["entry"]] = r
            for c in filter(None, r["callees"].split(",")):
                callers[c].append(r["entry"])
    strings = collections.defaultdict(set)
    with open(os.path.join(D, "literals.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
            s = r["string"].strip()
            if len(s) >= 3:
                strings[r["func"]].add(s[:40])
    for line in open(sys.argv[1], encoding="utf-8"):
        if line.startswith("#"):
            continue
        e, name, *consts = line.split()
        cs = callers.get(e, [])
        strs = set(strings.get(e, ()))
        for c in cs[:6]:
            strs |= {"^" + s for s in strings.get(c, ())}
        print(f"{e} sz={funcs[e]['size']:>5} callers={len(cs):<3} {' '.join(consts):28s} "
              f"from={','.join(c[2:] for c in cs[:4])} strs={sorted(strs)[:4]}")


if __name__ == "__main__":
    main()
