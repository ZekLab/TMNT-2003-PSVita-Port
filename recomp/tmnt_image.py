"""TMNT 2003 PC image for the repentogxm translator: fixed base 0x400000 (no .reloc), every executable section is code."""
import os
import sys

REF = os.path.join(os.path.dirname(__file__), "xlat")
sys.path.insert(0, os.path.abspath(REF))

import trans as T  # noqa: E402
from image import Image  # noqa: E402

# repentogxm pins Isaac-specific noreturn sites by address; none of them apply here.
for _name in ("NORETURN_IMPORTS", "NORETURN_WRAPPERS", "NORETURN_THUNKS", "NORETURN_INTERNALS",
              "NORETURN_INTERNAL_LONGJMPS", "NORETURN_SITES", "NORETURN_TERMINATIONS",
              "SWITCH_NORETURN_TERMINATIONS"):
    if hasattr(T, _name):
        setattr(T, _name, {})

# CRT helper 0x56b405 returns its result in the flags (`call 0x56b405; je special`, 6 sites): its two `ret`s keep them live.
FLAG_RETURNING = frozenset({0x16b405})
T.FLAG_RETURN_RETS = frozenset({0x16b416, 0x16b447})

# TMNT_EXE (tools/make_vita_files.py) points at the user's copy anywhere on disk.
EXE = os.path.abspath(os.environ.get("TMNT_EXE") or os.path.join(os.path.dirname(__file__), "..", "game", "app", "TMNT_EN.EXE"))


# Code patches applied before translation: RVA -> (expected bytes, new bytes, reason). The exe on disk stays untouched.
CODE_PATCHES = {
    # Title screen idle counter: `cmp dword ptr [eax], 0x708` (1800 frames, then the attract demo via 0x4981e1); never reached now.
    0x9865a: (b"\x08\x07\x00\x00", b"\xff\xff\xff\xff", "no attract demo on the PRESS START screen"),
}


class TmntImage(Image):
    def __init__(self, path=EXE):
        super().__init__(path, newbase=0x400000)
        mem = bytearray(self.mem)
        for rva, (old, new, why) in CODE_PATCHES.items():
            if bytes(mem[rva:rva + len(old)]) != old:
                raise ValueError("code patch at %x (%s): unexpected bytes" % (rva, why))
            mem[rva:rva + len(new)] = new
        self.mem = bytes(mem)
        self.code_ranges = [(s["rva"], s["rva"] + s["vsize"]) for s in self.sections if s["exec"]]

    def is_code(self, rva):
        return any(a <= rva < b for a, b in self.code_ranges)
