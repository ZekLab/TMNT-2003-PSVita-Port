"""Function discovery for TMNT_EN.EXE: recursive descent from the entry point, data code-pointers and immediate code-pointers, with jump-table resolution.

Writes out/functions.json: {"functions": [{"rva", "extra", "tables"}], "stats": {...}}.
"""
import collections
import json
import os
import struct
import sys

import tmnt_image as TI
import trans as T
from capstone import x86 as cx

OUT = os.path.join(os.path.dirname(__file__), "out")
IMPOSSIBLE = {
    "aaa", "aas", "aam", "aad", "daa", "das", "into", "bound", "salc", "xlatb", "lds", "les", "arpl",
    "sysenter", "sysexit", "hlt", "in", "out", "insb", "insd", "outsb", "outsd", "iretd", "retf",
}


def u32(img, rva):
    return struct.unpack_from("<I", img.mem, rva)[0]


def data_sections(img):
    return [(s["rva"], s["rva"] + s["vsize"]) for s in img.sections if not s["exec"] and s["name"] in (".rdata", ".data", "_rwdseg")]


def code_va(img, v):
    rva = (v - img.base) & 0xFFFFFFFF
    return rva if img.is_code(rva) else None


def jump_table(img, ins):
    """`jmp dword ptr [reg*4 + table]` -> (table rva, [target rvas]) read until an entry leaves the code."""
    if len(ins.operands) != 1 or ins.operands[0].type != cx.X86_OP_MEM:
        return None
    m = ins.operands[0].mem
    if m.scale != 4 or m.base != 0 or m.index == 0:
        return None
    t = (m.disp - img.base) & 0xFFFFFFFF
    if not (0 <= t < img.size - 4):
        return None
    targets = []
    k = 0
    while t + 4 * k + 4 <= img.size and k < 1024:
        r = code_va(img, u32(img, t + 4 * k))
        if r is None or abs(r - ins.address) > 0x10000:
            # The CRT memcpy tables are indexed from 1: an unused slot 0 is not the end of the table.
            if k == 0:
                k += 1
                continue
            break
        targets.append(r)
        k += 1
    return (t, targets) if targets else None


def decode_with_tables(img, start):
    extra = set()
    tables = {}
    edges = {}
    while True:
        insns, order, indirect = T.decode(img, start, extra_starts=tuple(sorted(extra)))
        grown = False
        for r in indirect:
            ins = insns[r]
            if ins.mnemonic != "jmp":
                continue
            jt = jump_table(img, ins)
            if jt is None:
                continue
            tables[jt[0]] = len(jt[1])
            edges[r] = jt[1]
            new = set(jt[1]) - extra
            if new:
                extra |= new
                grown = True
        if not grown:
            return insns, order, indirect, extra, tables, edges


def main():
    img = TI.TmntImage()
    roots = {img.entry: "entry"}
    for a, b in data_sections(img):
        for off in range(a, b - 3, 4):
            r = code_va(img, u32(img, off))
            if r is not None:
                roots.setdefault(r, "data")
    work = list(roots)
    funcs = {}
    failed = {}
    table_spans = []
    imm_ptrs = 0
    while work:
        rva = work.pop()
        if rva in funcs or rva in failed:
            continue
        try:
            insns, order, indirect, extra, tables, edges = decode_with_tables(img, rva)
        except T.Unsupported as ex:
            failed[rva] = str(ex)
            continue
        bad = [insns[r].mnemonic for r in order[:40] if insns[r].mnemonic in IMPOSSIBLE]
        if bad and roots.get(rva) in ("data", "imm"):
            failed[rva] = "data pointer decodes %s" % bad[0]
            continue
        funcs[rva] = {"rva": rva, "insns": len(order), "extra": sorted(extra), "tables": tables, "edges": {"%x" % k: v for k, v in edges.items()}, "indirect": len(indirect), "via": roots.get(rva, "call")}
        for t, n in tables.items():
            table_spans.append((t, t + 4 * n))
        for r in order:
            ins = insns[r]
            if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == cx.X86_OP_IMM:
                if img.is_code(ins.operands[0].imm):
                    work.append(ins.operands[0].imm)
            # Branches that leave the decoded body (beyond the decode limit, or shared tails) become functions of their own.
            if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == cx.X86_OP_IMM:
                t = ins.operands[0].imm
                if t not in insns and img.is_code(t):
                    roots.setdefault(t, "branch")
                    work.append(t)
            for o in ins.operands:
                if o.type == cx.X86_OP_IMM and ins.mnemonic not in ("call",) and not ins.mnemonic.startswith("j"):
                    c = code_va(img, o.imm)
                    if c is not None and c not in funcs:
                        imm_ptrs += 1
                        roots.setdefault(c, "imm")
                        work.append(c)

    # Data-root candidates that are really jump-table entries or interior labels of decoded functions.
    in_table = lambda r: False
    interior = set()
    for f in funcs.values():
        interior.update(f["extra"])
    dropped = [r for r in list(funcs) if funcs[r]["via"] == "data" and r in interior]
    for r in dropped:
        del funcs[r]

    via = collections.Counter(f["via"] for f in funcs.values())
    reasons = collections.Counter(v.split(":")[0][:60] for v in failed.values())
    stats = {
        "functions": len(funcs), "by_root": dict(via), "jump_tables": sum(len(f["tables"]) for f in funcs.values()),
        "functions_with_unresolved_indirect_jmp": sum(1 for f in funcs.values() if f["indirect"] > len(f["tables"])),
        "rejected_candidates": len(failed), "dropped_interior": len(dropped), "instructions": sum(f["insns"] for f in funcs.values()),
    }
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "functions.json"), "w") as f:
        json.dump({"functions": sorted(funcs.values(), key=lambda x: x["rva"]), "failed": {"%x" % k: v for k, v in failed.items()}, "stats": stats}, f, indent=1)
    print(json.dumps(stats, indent=1))
    for r, n in reasons.most_common(8):
        print("  rejected %5d  %s" % (n, r))


if __name__ == "__main__":
    sys.exit(main())
