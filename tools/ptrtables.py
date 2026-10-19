"""Find tables of function pointers in the image (runs of >= MIN consecutive words that point
at known function entries; null words allowed inside a run).

usage: python -I ptrtables.py [min_run]  -> writes facts/ptrtables.txt
"""
import csv
import os
import struct
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BASE = 0x8C010000


def main():
    min_run = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    funcs = {}
    with open(os.path.join(SC, "facts", "functions.tsv"), encoding="utf-8") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            funcs[int(r["entry"], 16)] = r["name"]
    img = open(os.path.join(SC, "extracted", "1ST_READ.BIN"), "rb").read()
    out = []
    i = 0
    n = len(img) // 4
    while i < n:
        j = i
        hits = 0
        while j < n:
            v = struct.unpack_from("<I", img, j * 4)[0]
            if v in funcs:
                hits += 1
            elif v != 0:
                break
            j += 1
        if hits >= min_run:
            entries = [struct.unpack_from("<I", img, k * 4)[0] for k in range(i, j)]
            out.append((BASE + i * 4, entries))
            i = j
        else:
            i = j + 1 if hits == 0 else i + 1
    with open(os.path.join(SC, "facts", "ptrtables.txt"), "w", encoding="utf-8") as o:
        for addr, entries in out:
            o.write(f"{addr:08x} n={len(entries)}: " + " ".join(f"{e:08x}" if e else "0" for e in entries) + "\n")
    print(len(out), "tables")


if __name__ == "__main__":
    main()
