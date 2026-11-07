"""Write every discovered function as C (out/c/guest_NNN.c, CHUNK functions per file) with the repentogxm emitter."""
import json
import os
import sys

import tmnt_image as TI
import build_one as B
from capstone import x86 as cx

OUT = os.path.join(os.path.dirname(__file__), "out")
CHUNK = 200

# Functions replaced by a hand-written body: RVA -> (reason, C statements). Each body must leave the guest stack as the original's `ret` would.
# Host hooks run at the entry of a translated function (before its body): RVA -> host function name, declared in rt/game_hooks.h.
HOOKS = {
    0x011d93: "tmnt_hook_start_area",
    0x0047d7: "tmnt_hook_set_next_area",
    0x098466: "tmnt_hook_title",
    0x09e126: "tmnt_hook_pad_settings",
    0x0aca5f: "tmnt_hook_logic",
    0x0ac384: "tmnt_hook_draw",
    0x0b000b: "tmnt_hook_present",
    0x0af87e: "tmnt_hook_skip_check",
    0x13ed10: "tmnt_hook_sfd_frame",    # mwPlyFxCnvFrmARGB8888(handle, frame info, out): which Sofdec frame is shown
}

# Host fast paths tried at the entry of a translated function (before its body): a nonzero return means the host did the work
# (registers and the `ret` included), zero runs the translated body as usual. RVA -> host function name, in rt/game_hooks.h.
FASTPATHS = {
    0x16abe9: "tmnt_fast_getptd",   # CRT _getptd: ~2300 calls a frame in fights, three API calls each
    0x0b3354: "tmnt_fast_ticks",    # elapsed rdtsc ticks: ~14500 calls a frame in fights, through two more translated calls
    0x0f52f0: "tmnt_fast_skin4",    # RpSkin 4-bone blend: 26% of a drawn frame's translated work in fights
    0x0f5960: "tmnt_fast_skin2",    # RpSkin 2-bone blend: 3.6%
    0x0f5880: "tmnt_fast_skin1",    # RpSkin, one bone for the whole mesh: 1.2%
    0x1850ef: "tmnt_fast_rw_xform_point",   # RwV3dTransformPoint (x87), per silhouette vertex and in logic
    0x0b8de0: "tmnt_fast_rw_invsqrt",       # RW table reciprocal square root
    0x0fca50: "tmnt_fast_toon_ink_vertex",  # RpToon ink vertex of a silhouette edge
    0x0fd570: "tmnt_fast_toon_ink_quad",    # RpToon ink quad appended to the vertex buffer
    0x0b8d10: "tmnt_fast_rw_v3d_length",    # RwV3dLength: 4.8% of a logic tick, ~850 calls
    0x185000: "tmnt_fast_rw_xform_points",  # RwV3dTransformPoints (x87): 2.4% of logic
    0x1852a0: "tmnt_fast_rw_matrix_multiply",   # RwMatrixMultiply (x87): 5.5% of logic, 2.2% of a drawn frame
    0x0a64aa: "tmnt_fast_anim_curve",       # the game's animation curve (search + cubic): 14.4% of a logic tick
    0x0ef9f0: "tmnt_fast_hanim_attach",     # RpHAnim attach over the frame tree, every character every tick: ~8% of logic
    0x118cd0: "tmnt_fast_sphere_tri",       # sphere/triangle collision, early rejection only: ~6% of logic
    0x0cc050: "tmnt_fast_rw_flush_states",  # RwD3D9 render/texture stage state flush: thousands of COM calls a frame in fights
    0x1593b0: "tmnt_fast_sfd_yuv_argb",     # Sofdec movie frame YUV -> ARGB: only SSE/MMX versions exist, so movies stayed black
    # Sofdec's own picture is no longer shown (pl_mpeg draws movies): its IDCT and motion compensation become no-ops, so the
    # bitstream is still parsed (pacing, sound) without the decoding work that left no CPU for pl_mpeg on the Vita.
    0x15bf50: "tmnt_fast_sfd_skip",         # IDCT, plain C version
    0x186ee0: "tmnt_fast_sfd_skip",         # motion compensation, plain C versions
    0x1872f0: "tmnt_fast_sfd_skip",
    0x1873c0: "tmnt_fast_sfd_skip",
    0x187480: "tmnt_fast_sfd_skip",
}

# False function entries whose decoding reads flags after a call (never executed): RVA -> reason. Skipped by the flag check.
FLAG_CHECK_IGNORE = {
    0x13009f: "data words 0x0053/0x009f at 0x5e640c read as a code pointer; starts inside `mov esi, [esi+0x74]` at 0x53009d",
    0x1800a0: "data at 0x5e641c read as a code pointer; starts inside `push 3` at 0x58009f",
    0x1800a3: "data at 0x6042d0 read as a code pointer; starts inside `push 1` at 0x5800a2",
}

# Native replacements of hot loops inside translated functions: start RVA -> (end RVA, host function, in rt/game_hooks.h).
# At `start` the host function sees the published registers; a nonzero return means it left registers, stack locals and memory
# as the translated code would at `end`, which follows. `<name>_check(c)` runs at `end` (TMNT_HLE_CHECK=1 compares there).
REGIONS = {
    0x0fc1ef: (0x0fc2ac, "tmnt_region_toon_face_normals"),   # RpToon brute silhouette: face normals, 36% of 0x4fc170
    0x0fc322: (0x0fc380, "tmnt_region_toon_cam_vectors"),    # vertex-to-camera vectors, 14%
    0x0fc427: (0x0fc49d, "tmnt_region_toon_facing"),         # front/back facing per face (x87 path), 21%
    0x0fc50d: (0x0fc610, "tmnt_region_toon_edges"),          # silhouette edge loop with its ink vertices and quads, 28%
    0x0f88dd: (0x0f8907, "tmnt_region_toon_copy_pos"),       # 0x4f86d0: strided vertex positions into a packed array
    0x0f894f: (0x0f8979, "tmnt_region_toon_copy_nrm"),       # 0x4f86d0: strided normals into a packed array
    0x0b1a4e: (0x0b1a53, "tmnt_region_pad_label"),           # Game pad page: "BUTTON n" label drawn as the Vita button
}

OVERRIDES = {
    # Bodies use the CPU fields directly (no GUEST_GPR_DECL), so they hold in both register modes; `c->esp += 4` is the `ret`.
    0x1630a0: ("CPU probe: report no SSE (the CRI ADX SSE decoder also uses MMX)", ["c->eax = 0;", "c->esp += 4U;"]),
    0x163110: ("CPU probe: report no MMX", ["c->eax = 0;", "c->esp += 4U;"]),
}


# x87 stack in locals (guest.h GUEST_X87_LOCAL): functions that touch the x87 stack are spelled with the X* forms and seams that
# also publish/reload the stack. Functions that save or restore the whole FPU state keep the CPU-struct forms.
X87_MARKERS = ("fpush(c, ", "fpop(c)", "fst(c, ", "c->st[_di]")
X87_KEEP = ("guest_fnsave", "guest_frstor", "guest_fnstenv", "guest_fldenv", "c->st_top = ")


def x87_locals(lines):
    text = "\n".join(lines)
    if not any(m in text for m in X87_MARKERS) or any(k in text for k in X87_KEEP):
        return lines
    for a, b in (("GUEST_GPR_DECL;", "GUEST_GPR_DECL; GUEST_X87_DECL;"), ("GUEST_GPR_FLUSH(c)", "GUEST_X87_GPR_FLUSH(c)"),
                 ("GUEST_GPR_RELOAD(c)", "GUEST_X87_GPR_RELOAD(c)"), ("fpush(c, ", "XPUSH("), ("fpop(c)", "XPOP()"),
                 ("fst(c, ", "XST("), ("(c->st_top + ", "(_xt + "), ("c->st[_di]", "_x87[_di]")):
        text = text.replace(a, b)
    assert "c->st" not in text, "x87 locals: direct stack access left"
    return text.split("\n")


def function_lines(em, body, order, start, name):
    L = ["void %s(CPU *__restrict c)" % name, "{"]
    L.append("    GUEST_ENTER(0x%08xU);" % start)
    if start in FASTPATHS:
        L.append("    if (%s(c)) return;" % FASTPATHS[start])
    if start in HOOKS:
        L.append("    %s(c);" % HOOKS[start])
    L.extend("    %s" % line for line in B.flag_state_prologue(em))
    gate = B.entry_gate(start, order)
    if gate:
        L.append("    %s" % gate)
    here = {ins.address for ins, _ in body}
    regions = {s: r for s, r in REGIONS.items() if s in here and r[0] in here}
    ends = {r[0]: r[1] for r in regions.values()}
    em.labels.update(ends)
    for ins, stmts in body:
        if ins.address in em.labels:
            L.append("L_%08x:" % ins.address)
        if ins.address in ends:
            L.append("    GUEST_GPR_FLUSH(c); %s_check(c); GUEST_GPR_RELOAD(c);" % ends[ins.address])
        if ins.address in regions:
            end, fn = regions[ins.address]
            L.append("    GUEST_GPR_FLUSH(c); if (%s(c)) { GUEST_GPR_RELOAD(c); goto L_%08x; } GUEST_GPR_RELOAD(c);" % (fn, end))
        L.append("    /* %08x  %s %s */" % (ins.address, ins.mnemonic, ins.op_str))
        L.extend("    %s" % s for s in stmts)
    L.append("}")
    return x87_locals(L)


def main():
    img = TI.TmntImage()
    with open(os.path.join(OUT, "functions.json")) as f:
        funcs = json.load(f)["functions"]
    known = {f["rva"] for f in funcs}
    flag_sites, flag_bad = [], []
    cdir = os.path.join(OUT, "c")
    os.makedirs(cdir, exist_ok=True)
    for n in range(0, len(funcs), CHUNK):
        part = funcs[n:n + CHUNK]
        decls, bodies = set(), []
        for fn in part:
            rva = fn["rva"]
            name = "sub_%08x" % rva
            if rva in OVERRIDES:
                why, stmts = OVERRIDES[rva]
                bodies.extend(["/* override: %s */" % why, "void %s(CPU *__restrict c)" % name, "{"] + ["    " + x for x in stmts] + ["}", ""])
                continue
            em, insns, order, body, unsup, indirect, members = B.translate(img, rva, name, known=known, extra_entries=fn["extra"], indirect_edges={int(k, 16): v for k, v in fn.get("edges", {}).items()})
            for site in ({} if rva in FLAG_CHECK_IGNORE else em.call_flag_reads):
                op = insns[site].operands[0]
                if op.type != cx.X86_OP_IMM or op.imm not in TI.FLAG_RETURNING:
                    flag_bad.append("flags read after the call at %08x (%s) in %08x, reads %s"
                                    % (site + 0x400000, insns[site].op_str, rva + 0x400000, "".join(sorted(em.call_flag_reads[site]))))
                else:
                    flag_sites.append(site)
            decls.update(em.calls)
            bodies.extend(function_lines(em, body, order, rva, name))
            bodies.append("")
        L = ["/* GENERATED by recomp/gen_c.py from TMNT_EN.EXE -- do not edit. */", "#include <math.h>", "#include \"guest.h\"", "#include \"game_hooks.h\"", ""]
        L.extend("void sub_%08x(CPU *__restrict c);" % t for t in sorted(decls | {f["rva"] for f in part}))
        L.append("")
        L.extend(bodies)
        # Unchanged files keep their timestamps, so a rebuild only recompiles what changed (the Vita build takes ~10 minutes).
        path, text = os.path.join(cdir, "guest_%03d.c" % (n // CHUNK)), "\n".join(L) + "\n"
        if not os.path.exists(path) or open(path).read() != text:
            with open(path, "w") as f:
                f.write(text)
    print("files:", (len(funcs) + CHUNK - 1) // CHUNK, "flag-reading call sites:", len(flag_sites))
    if flag_bad:
        print("\n".join(flag_bad))
        raise SystemExit("callees not in FLAG_RETURNING (tmnt_image.py) or FLAG_CHECK_IGNORE")


if __name__ == "__main__":
    sys.exit(main())
