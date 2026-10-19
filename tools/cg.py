"""Call-graph helper over facts/functions.tsv and facts/literals.tsv.

usage:
  cg.py gaps                     address gaps > 4 KiB between functions (module boundaries)
  cg.py fn <addr> [depth]        callers and callees tree of a function
  cg.py lits <addr>              literals (ints/floats/strings) a function loads
  cg.py float <value> [...]      functions that load a given float constant
  cg.py str <substring>          functions that reference strings containing substring
"""
import csv
import os
import sys

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "facts")


def load():
    funcs, callers = {}, {}
    with open(os.path.join(D, "functions.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            r["callees"] = [c for c in r["callees"].split(",") if c]
            funcs[r["entry"]] = r
    for e, r in funcs.items():
        for c in r["callees"]:
            callers.setdefault(c, []).append(e)
    return funcs, callers


def lits():
    with open(os.path.join(D, "literals.tsv"), encoding="utf-8") as f:
        yield from csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE)


def norm(a):
    return a.lower().replace("0x", "").zfill(8)


def main():
    cmd = sys.argv[1]
    funcs, callers = load()
    names = {e: r["name"] for e, r in funcs.items()}
    if cmd == "gaps":
        es = sorted(funcs, key=lambda e: int(e, 16))
        for a, b in zip(es, es[1:]):
            end = int(a, 16) + int(funcs[a]["size"])
            if int(b, 16) - end > 0x1000:
                print(f"gap {end:08x}-{b} ({(int(b, 16) - end) // 1024} KiB)")
    elif cmd == "fn":
        a = norm(sys.argv[2])
        depth = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        r = funcs[a]
        print(f"{a} {r['name']} size={r['size']}")
        print("  callers:", " ".join(f"{c}:{names[c]}" for c in callers.get(a, [])))

        def tree(e, d, seen):
            for c in funcs[e]["callees"]:
                print("  " * (depth - d + 2) + f"{c} {names.get(c, '?')} ({funcs.get(c, {}).get('size')})")
                if d > 1 and c in funcs and c not in seen:
                    seen.add(c)
                    tree(c, d - 1, seen)
        tree(a, depth, {a})
    elif cmd == "lits":
        a = norm(sys.argv[2])
        for r in lits():
            if r["func"] == a:
                print(r["insn"], r["value"], r["float"], r["string"])
    elif cmd == "float":
        want = {float(x) for x in sys.argv[2:]}
        hits = {}
        for r in lits():
            try:
                if float(r["float"]) in want:
                    hits.setdefault(r["func"], set()).add(r["float"])
            except ValueError:
                pass
        for e in sorted(hits):
            print(e, names.get(e), funcs.get(e, {}).get("size"), sorted(hits[e]))
    elif cmd == "str":
        s = sys.argv[2].lower()
        for r in lits():
            if s in r["string"].lower():
                print(r["func"], r["insn"], r["string"][:100])


if __name__ == "__main__":
    main()
