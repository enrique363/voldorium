"""Packs a broad-oracle JSONL (capture_oracle_broad.sh) into the binary file read by replay.c.

usage: python -I oracle_pack.py <oracle_broad.jsonl> <out.bin>

Layout (little-endian u32): "SCOR", test count, then per test:
  fn, in-state[76], out-state[76], rd run count, wr run count, hw event count,
  rd runs (offset, length, bytes padded to 4), wr runs (same), hw events (type, addr, size, value)
State: r[16], fr[16], xf[16], pr, mach, macl, gbr, fpul, fpscr, t, s, q, m, sr, timer, sq[16].
Skipped calls (interrupt, SyncTest RAM edits, overflow) are counted, not packed.
"""
import collections
import json
import struct
import sys

ST_KEYS = ("pr", "mach", "macl", "gbr", "fpul", "fpscr", "t", "s", "q", "m", "sr", "timer")


def state(s):
    v = [int(x, 16) for x in s["r"] + s["fr"] + s["xf"]]
    for k in ST_KEYS:
        x = s[k]
        v.append(int(x, 16) if isinstance(x, str) else int(x))
    v.extend(struct.unpack("<16I", bytes.fromhex(s["sq"])))   # store-queue buffers SQ0/SQ1
    return v


def runs(rs):
    out = []
    for off, hx in rs:
        b = bytes.fromhex(hx)
        out.append(struct.pack("<II", int(off, 16), len(b)) + b + b"\0" * (-len(b) % 4))
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    tests, skipped, bad = [], collections.Counter(), 0
    for line in open(src, encoding="utf-8"):
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            bad += 1   # last line cut off when the emulator was stopped
            continue
        if "skip" in d:
            skipped[d["skip"]] += 1
            continue
        rd, wr = runs(d["rd"]), runs(d["wr"])
        hw = [struct.pack("<IIII", t, int(a, 16), sz, int(v, 16)) for t, a, sz, v in d["hw"]]
        head = struct.pack("<I", int(d["fn"], 16)) + struct.pack("<76I", *state(d["in"])) + \
            struct.pack("<76I", *state(d["out"])) + struct.pack("<III", len(rd), len(wr), len(hw))
        tests.append(head + b"".join(rd) + b"".join(wr) + b"".join(hw))
    with open(dst, "wb") as o:
        o.write(b"SCOR" + struct.pack("<I", len(tests)))
        for t in tests:
            o.write(t)
    fns = len({struct.unpack_from("<I", t)[0] for t in tests})
    print(f"packed {len(tests)} calls of {fns} functions; skipped {sum(skipped.values())}: {dict(skipped)}; unreadable lines {bad}")


if __name__ == "__main__":
    main()
