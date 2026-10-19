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

EXE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "game", "app", "TMNT_EN.EXE"))


class TmntImage(Image):
    def __init__(self, path=EXE):
        super().__init__(path, newbase=0x400000)
        self.code_ranges = [(s["rva"], s["rva"] + s["vsize"]) for s in self.sections if s["exec"]]

    def is_code(self, rva):
        return any(a <= rva < b for a, b in self.code_ranges)
