"""Finds functions that are only reached through tables of function pointers.

Ghidra finds functions through direct calls; routines a game mode stores in a table (state
handlers, menu actions) are missed, and the port stops with "no translated function" when the
game calls one. This scans the image for runs of words that point into the code blocks
(sh4c.RO), with nulls allowed inside, that hold at least one known entry. Every other word in
such a run is taken as a function entry when the code there decodes cleanly: each reachable
instruction is one sh4c knows, none is privileged, and the walk ends in a return or jump. Code addresses
loaded from literal pools (callbacks stored at run time) are checked the same way. New entries
anchor further tables, so it repeats until nothing new turns up.

usage: python -I tools/find_entries.py [--write]
  prints the new entries; --write appends them to port/sh4c/extra_entries.txt
"""
import json
import os
import struct
import sys

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(SC, "port", "sh4c"))
import sh4c  # noqa: E402

EXTRA = os.path.join(SC, "port", "sh4c", "extra_entries.txt")
PRIVILEGED = ("ldc", "stc", "rte", "sleep", "ldtlb", "trapa")


def known_entries():
    funcs = json.load(open(os.path.join(SC, "port", "functions.json")))
    k = {int(f["entry"], 16) for f in funcs}
    for line in open(EXTRA):
        t = line.split("#")[0].strip()
        if t:
            k.add(int(t, 16))
    return k


def is_code(t, cache={}):
    if t in cache:
        return cache[t]
    ok = False
    if sh4c.ro(t) and not t & 1:
        try:
            _, reach = sh4c.recover(t, [])
        except Exception:
            reach = set()
        ok = bool(reach) and len(reach) < 20000
        ends = False
        for a in reach:
            name = sh4c.decode(sh4c.u16(a))[0]
            if name is None or name.startswith(PRIVILEGED):
                ok = False
                break
            if name in ("rts", "jmp", "bra", "braf"):
                ends = True
        ok = ok and ends
    cache[t] = ok
    return ok


def scan(known):
    img = sh4c.IMG
    n = len(img) // 4
    words = struct.unpack_from("<%dI" % n, img)
    found = set()
    i = 0
    while i < n:
        j = i
        while j < n and (words[j] == 0 or (sh4c.ro(words[j]) and not words[j] & 1)):
            j += 1
        run = [w for w in words[i:j] if w]
        if len(run) >= 2 and any(w in known for w in run):
            for w in run:
                if w not in known and is_code(w):
                    found.add(w)
        i = j + 1
    return found


def literals(known):
    """Code addresses loaded by `mov.l @(disp,pc),rn` in known functions (callbacks stored at run
    time); one that decodes cleanly is an entry, even inside a function body (a secondary entry)."""
    found = set()
    for e in sorted(known):
        try:
            _, reach = sh4c.recover(e, [])
        except Exception:
            continue
        for a in reach:
            op = sh4c.u16(a)
            if op >> 12 == 0xD:
                v = sh4c.u32((a & ~3) + 4 + (op & 0xFF) * 4)
                if v not in known and is_code(v):
                    found.add(v)
    return found


def main():
    known = known_entries()
    sh4c.ENTRIES.update(known)
    new = set()
    while True:
        f = (scan(known | new) | literals(known | new)) - known - new
        if not f:
            break
        new |= f
        sh4c.ENTRIES.update(f)
    for t in sorted(new):
        print("%08x" % t)
    print("%d new entries" % len(new), file=sys.stderr)
    if "--write" in sys.argv and new:
        with open(EXTRA, "a") as o:
            o.write("# found in tables of function pointers (tools/find_entries.py)\n")
            for t in sorted(new):
                o.write("%08x\n" % t)


if __name__ == "__main__":
    main()
