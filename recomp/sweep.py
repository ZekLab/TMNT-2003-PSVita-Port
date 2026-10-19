"""Translate every discovered function with the repentogxm emitter and report what does not translate.

Writes out/sweep.json and prints the summary.
"""
import collections
import json
import os
import sys
import time

import tmnt_image as TI
import build_one as B
import trans as T

OUT = os.path.join(os.path.dirname(__file__), "out")


def main():
    img = TI.TmntImage()
    with open(os.path.join(OUT, "functions.json")) as f:
        funcs = json.load(f)["functions"]
    known = {f["rva"] for f in funcs}
    ok = bad = crashed = 0
    insn_ok = insn_bad = 0
    reasons = collections.Counter()
    mnems = collections.Counter()
    per_func = {}
    t0 = time.time()
    for i, fn in enumerate(funcs):
        rva = fn["rva"]
        try:
            em, insns, order, body, unsup, indirect, members = B.translate(img, rva, "sub_%08x" % rva, known=known, extra_entries=fn["extra"], indirect_edges={int(k, 16): v for k, v in fn.get("edges", {}).items()})
        except T.Unsupported as ex:
            crashed += 1
            reasons["decode: " + str(ex)[:70]] += 1
            continue
        except Exception as ex:  # noqa: BLE001
            crashed += 1
            reasons["crash: %s %s" % (type(ex).__name__, str(ex)[:60])] += 1
            continue
        if unsup:
            bad += 1
            insn_bad += len(order)
            per_func[rva] = [str(u)[:80] for u in unsup[:3]]
            for u in unsup:
                ins = getattr(u, "ins", None) or (u.args[0] if u.args else None)
                m = getattr(ins, "mnemonic", "?")
                mnems[m] += 1
                reasons[str(u).split("--")[-1].strip()[:70]] += 1
        else:
            ok += 1
            insn_ok += len(order)
        if i % 2000 == 0:
            print("  %d/%d  %.0f s" % (i, len(funcs), time.time() - t0), flush=True)
    stats = {"functions": len(funcs), "clean": ok, "with_unsupported": bad, "crashed": crashed,
             "insns_clean": insn_ok, "insns_with_unsupported": insn_bad, "seconds": round(time.time() - t0)}
    with open(os.path.join(OUT, "sweep.json"), "w") as f:
        json.dump({"stats": stats, "mnemonics": mnems.most_common(), "reasons": reasons.most_common(),
                   "functions": {"%x" % k: v for k, v in per_func.items()}}, f, indent=1)
    print(json.dumps(stats, indent=1))
    print("unsupported instructions by mnemonic:")
    for m, n in mnems.most_common(25):
        print("  %6d  %s" % (n, m))
    print("reasons:")
    for r, n in reasons.most_common(15):
        print("  %6d  %s" % (n, r))


if __name__ == "__main__":
    sys.exit(main())
