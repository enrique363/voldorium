"""Scan decompiled C for screen-space constants (int and float forms) per function.

usage: python -I scan2d.py <decomp_dir>   -> TSV: func, counts per constant, first matching lines
"""
import os
import re
import sys

PATS = {
    "640": r"(?<![\w.])(?:640\.0|0x280|640|0x44200000)(?![\w.])",
    "639": r"(?<![\w.])(?:639\.0|0x27f|639)(?![\w.])",
    "480": r"(?<![\w.])(?:480\.0|0x1e0|480|0x43f00000)(?![\w.])",
    "479": r"(?<![\w.])(?:479\.0|0x1df|479)(?![\w.])",
    "320": r"(?<![\w.])(?:320\.0|0x140|0x43a00000)(?![\w.])",
    "240": r"(?<![\w.])(?:240\.0|0x43700000)(?![\w.])",
}
RX = {k: re.compile(v) for k, v in PATS.items()}


def main():
    d = sys.argv[1]
    print("func\tname\t640\t639\t480\t479\t320\t240\tsample")
    for fn in sorted(os.listdir(d)):
        text = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
        counts = {k: len(r.findall(text)) for k, r in RX.items()}
        if not (counts["640"] or counts["639"] or counts["480"] or counts["479"] or counts["320"]):
            continue
        sample = ""
        for line in text.splitlines():
            if RX["640"].search(line) or RX["480"].search(line) or RX["320"].search(line):
                sample = line.strip()[:90]
                break
        e, _, name = fn[:-2].partition("_")
        print(f"{e}\t{name}\t" + "\t".join(str(counts[k]) for k in PATS) + f"\t{sample}")


if __name__ == "__main__":
    main()
