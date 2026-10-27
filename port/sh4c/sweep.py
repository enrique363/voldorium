"""Translate every function in functions.json and report what the translator cannot handle yet.

usage: python -I sweep.py <out_dir> [parts]
Writes <out_dir>/all_<k>.c (functions split into `parts` files for parallel compiling) and
<out_dir>/sweep_report.json. The report only counts instructions reachable from each entry
(fallthrough, branches, delay slots), so literal pools decoded as code are ignored.
"""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh4c  # noqa: E402

# SH-4 instructions sh4c does not decode, by mask/value (names for the report only)
MISSING = [
    ("0000nnnnmmmm1111", "mac.l"), ("0100nnnnmmmm1111", "mac.w"), ("0100nnnn00011011", "tas.b"),
    ("11000011iiiiiiii", "trapa"), ("0000000000011011", "sleep"), ("0000000000111000", "ldtlb"),
    ("0000000000101011", "rte"), ("0000nnnn11000011", "movca.l"), ("0000nnnn10010011", "ocbi"),
    ("0000nnnn10100011", "ocbp"), ("0000nnnn10110011", "ocbwb"),
    ("0011nnnnmmmm1111", "addv"), ("0011nnnnmmmm1011", "subv"),
    ("11001101iiiiiiii", "and.b @(r0,gbr)"), ("11001111iiiiiiii", "or.b @(r0,gbr)"),
    ("11001110iiiiiiii", "xor.b @(r0,gbr)"), ("11001100iiiiiiii", "tst.b @(r0,gbr)"),
    ("0100nnnn00001110", "ldc sr"), ("0100nnnn00000111", "ldc.l sr"), ("0000nnnn00000010", "stc sr"),
    ("0100nnnn00000011", "stc.l sr"), ("0100nnnn00101110", "ldc vbr"), ("0000nnnn00100010", "stc vbr"),
    ("0100nnnn00100111", "ldc.l vbr"), ("0100nnnn00100011", "stc.l vbr"),
    ("0100nnnn00010111", "ldc.l gbr"), ("0100nnnn00010011", "stc.l gbr"),
    ("0100nnnn00111110", "ldc ssr"), ("0000nnnn00110010", "stc ssr"), ("0100nnnn01001110", "ldc spc"),
    ("0000nnnn01000010", "stc spc"), ("0000nnnn00111010", "stc sgr"), ("0100nnnn11111010", "ldc dbr"),
    ("0000nnnn11111010", "stc dbr"), ("0100nnnn1mmm1110", "ldc rm_bank"), ("0000nnnn1mmm0010", "stc rm_bank"),
    ("0100nnnn0mmm0111", "ldc.l ctl"), ("0100nnnn0mmm0011", "stc.l ctl"),
    ("0100nnnn1mmm0111", "ldc.l rm_bank"), ("0100nnnn1mmm0011", "stc.l rm_bank"),
]
MISSING_T = [(sh4c._compile(p), name) for p, name in MISSING]
TERMINAL = {"rts", "bra", "jmp", "braf"}


def missing_name(op):
    for (mask, val), name in MISSING_T:
        if op & mask == val:
            return name
    return f"undefined {op >> 12:x}xxx"


def walk(entry, ranges):
    """Reachable addresses (sh4c.recover) plus structural notes and outgoing calls."""
    _, reach = sh4c.recover(entry, ranges)
    notes = collections.Counter()
    calls = []
    for a in sorted(reach):
        name, n, m, i, d = sh4c.decode(sh4c.u16(a))
        if name == "bsr":
            calls.append(a + 4 + sh4c.s12(d) * 2)
        elif name == "braf":
            ok = sh4c.jump_table(a, n, sh4c._ReadOnly())
            notes["braf (jump table resolved)" if ok else "braf (unresolved)"] += 1
        elif name == "jmp":
            kind, ts = sh4c.jmp_targets(a, n)
            if kind == "table":
                notes["jmp through pointer table (goto)" if ts else "jmp through pointer table (unresolved)"] += 1
            elif kind == "register":
                notes["jmp via register (dispatched at run time)"] += 1
            elif ts[0] in sh4c.ENTRIES:
                notes["jmp to a function entry (tail call)"] += 1
            elif ts[0] in reach:
                notes["jmp inside the function (goto)"] += 1
            elif ts[0] < 0x8c010000 or ts[0] >= 0xa0000000:
                notes["jmp to BIOS (syscall vector / reset): HLE"] += 1
            else:
                notes["jmp to unknown code"] += 1
                calls.append(ts[0])
    body = {x for lo, hi in ranges for x in range(lo, hi + 1, 2)}
    if reach - body:
        notes["code recovered beyond Ghidra's body"] += 1
    return reach, notes, calls


def main():
    out_dir, parts = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 8
    os.makedirs(out_dir, exist_ok=True)
    funcs = json.load(open(os.path.join(sh4c.SC, "port", "functions.json")))
    entries = {int(f["entry"], 16) for f in funcs}
    sh4c.ENTRIES.update(entries)
    unimpl = collections.Counter()        # mnemonic -> reachable sites
    unimpl_fns = collections.defaultdict(set)
    notes = collections.Counter()
    note_fns = collections.defaultdict(list)
    bodies = []
    per_fn = {}
    for f in funcs:
        entry = int(f["entry"], 16)
        ranges = [(int(lo, 16), int(hi, 16)) for lo, hi in f["ranges"]]
        try:
            code = sh4c.translate(entry, ranges)
        except Exception as e:  # translator crash = structural gap
            notes["translator exception"] += 1
            note_fns["translator exception"].append(f"{entry:08x} {e!r}")
            code = f"void f_{entry:08x}(SH4 *c)\n{{\n\tsh4_unimpl(c, 0x{entry:08x}u, 0); return;\n}}"
        reach, fnotes, targets = walk(entry, ranges)
        if "falls through to" in code:
            fnotes["falls through into another function (tail call)"] += 1
        bad = []
        for a in sorted(reach):
            op = sh4c.u16(a)
            name = sh4c.decode(op)[0]
            if name is None:
                bad.append(missing_name(op))
            elif name not in sh4c.DELAYED and name not in ("bt", "bf") and sh4c.emit_simple(*sh4c.decode(op)[:5], a) is None:
                bad.append(name)
            elif name in sh4c.DELAYED and a + 2 in reach:
                dop = sh4c.u16(a + 2)
                dname = sh4c.decode(dop)[0]
                if dname in sh4c.DELAYED or dname in ("bt", "bf"):
                    bad.append("branch in delay slot")
        for b in bad:
            unimpl[b] += 1
            unimpl_fns[b].add(f["entry"])
        for k, v in fnotes.items():
            notes[k] += v
            note_fns[k].append(f["entry"])
        for t in targets:
            if t not in entries:
                k = "call to non-entry"
                notes[k] += 1
                note_fns[k].append(f"{f['entry']}->{t:08x}")
        per_fn[f["entry"]] = {"name": f["name"], "insns": len(reach), "unimpl": sorted(set(bad))}
        bodies.append((f["entry"], f["name"], code))
    # bsr targets that are not function entries get their own entry point (body recovered
    # by control flow from the target)
    extra, orphan = set(), []
    for item in note_fns.get("call to non-entry", []):
        t = int(item.split("->")[1], 16)
        if sh4c.ro(t):
            extra.add(t)
        else:
            orphan.append(item)
    # entry points only seen at run time (the broad oracle's indirect calls), one hex per line
    dyn = os.path.join(os.path.dirname(os.path.abspath(__file__)), "extra_entries.txt")
    if os.path.exists(dyn):
        for line in open(dyn):
            t = int(line.split("#")[0].strip() or "0", 16)
            if t and t not in entries:
                extra.add(t)
    # the added entries' own calls need entries too (0x8C032A36, called only from 0x8C032C60, an
    # extra entry: the training menu's FREESTYLE SETTINGS stopped there), until nothing new turns up
    sh4c.ENTRIES.update(extra)
    work = sorted(extra)
    while work:
        t = work.pop()
        for c in walk(t, [])[2]:
            if c not in entries and c not in extra and sh4c.ro(c):
                extra.add(c)
                sh4c.ENTRIES.add(c)
                work.append(c)
    for t in sorted(extra):
        bodies.append((f"{t:08x}", "secondary entry (call target inside a function)", sh4c.translate(t, [])))
    # dispatcher: address -> translated function
    all_entries = sorted(int(e, 16) for e, n, c in bodies)
    with open(os.path.join(out_dir, "dispatch.c"), "w") as o:
        o.write('#include "sh4rt.h"\n\n')
        for e in all_entries:
            o.write(f"void f_{e:08x}(SH4 *c);\n")
        o.write("\n/* sorted by address, for binary search in sh4_call/sh4_jump */\n")
        o.write("const struct sh4_fn sh4_fns[] = {\n")
        for e in all_entries:
            o.write(f"\t{{ 0x{e:08x}u, f_{e:08x} }},\n")
        o.write("};\nconst int sh4_nfns = sizeof(sh4_fns) / sizeof(sh4_fns[0]);\n")
    # write C in parts
    for k in range(parts):
        chunk = bodies[k::parts]
        with open(os.path.join(out_dir, f"all_{k}.c"), "w") as o:
            o.write('#include "sh4rt.h"\n\n')
            for e, n, code in chunk:
                o.write(f"/* {n} */\n{code}\n\n")
    clean = sum(1 for v in per_fn.values() if not v["unimpl"])
    report = {
        "functions": len(funcs),
        "clean_functions": clean,
        "reachable_insns": sum(v["insns"] for v in per_fn.values()),
        "unimpl_sites": dict(unimpl.most_common()),
        "unimpl_functions": {k: len(v) for k, v in sorted(unimpl_fns.items(), key=lambda kv: -len(kv[1]))},
        "unimpl_examples": {k: sorted(v)[:12] for k, v in unimpl_fns.items()},
        "structural": dict(notes.most_common()),
        "secondary_entries": [f"{t:08x}" for t in sorted(extra)],
        "unresolved_targets": orphan,
        "structural_examples": {k: v[:15] for k, v in note_fns.items()},
        "per_function": per_fn,
    }
    json.dump(report, open(os.path.join(out_dir, "sweep_report.json"), "w"), indent=1)
    print(f"{len(funcs)} functions, {clean} with no unimplemented reachable instruction")
    print("unimplemented (reachable sites / functions):")
    for k, v in unimpl.most_common():
        print(f"  {k:24s} {v:6d} {len(unimpl_fns[k]):5d}")
    print(f"secondary entries added: {len(extra)}, targets outside any function: {orphan}")
    print("structural:")
    for k, v in notes.most_common():
        print(f"  {k:32s} {v:6d}")


if __name__ == "__main__":
    main()
