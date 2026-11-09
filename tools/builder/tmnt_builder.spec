# PyInstaller spec of the TMNT Vita files builder (GitHub Actions: .github/workflows/builder.yml).
#   ZIG_DIR=<folder with zig and lib/include> MOVCONV=<movconv binary> pyinstaller tools/builder/tmnt_builder.spec
# Windows: one TMNT2003-Vita-Builder.exe; Mac: TMNT2003 Vita Builder.app. The translator and runtime headers travel as data.
import glob
import os
import sys

ROOT = os.path.abspath(os.path.join(SPECPATH, "..", ".."))
ZIG_DIR = os.environ.get("ZIG_DIR")

datas = [(p, "recomp") for p in glob.glob(os.path.join(ROOT, "recomp", "*.py"))]
datas += [(p, "recomp/xlat") for p in glob.glob(os.path.join(ROOT, "recomp", "xlat", "*.py"))]
datas += [(p, "rt") for p in glob.glob(os.path.join(ROOT, "rt", "*.h"))]
datas += [(os.path.join(ROOT, "rt", n), "rt") for n in ("code_so.c", "code_so_entry.c")]
datas += [(os.path.join(ROOT, "tools", "vita_code", "include"), "tools/vita_code/include")]
if ZIG_DIR:   # zig, its LICENSE and lib/include (clang's own headers) are all the compiling needs
    zig = "zig.exe" if sys.platform == "win32" else "zig"
    datas += [(os.path.join(ZIG_DIR, zig), "zig"), (os.path.join(ZIG_DIR, "LICENSE"), "zig"),
              (os.path.join(ZIG_DIR, "lib", "include"), "zig/lib/include")]

MOVCONV = os.environ.get("MOVCONV")   # the movie converter (tools/movies), built for this system
if MOVCONV:
    datas += [(MOVCONV, "movconv")]

# The translator is run from data files, so its imports are listed here.
hidden = ["capstone", "capstone.x86", "pefile", "bisect", "collections", "json", "re", "struct", "time", "__future__"]

a = Analysis([os.path.join(SPECPATH, "tmnt_builder.py")], pathex=[os.path.join(ROOT, "tools"), SPECPATH], datas=datas,
             hiddenimports=hidden, noarchive=False)
pyz = PYZ(a.pure)

if sys.platform == "darwin":
    exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="TMNT2003 Vita Builder", console=False)
    coll = COLLECT(exe, a.binaries, a.datas, name="TMNT2003 Vita Builder")
    app = BUNDLE(coll, name="TMNT2003 Vita Builder.app", bundle_identifier="io.github.zeklab.tmnt2003vitabuilder")
else:
    exe = EXE(pyz, a.scripts, a.binaries, a.datas, [], name="TMNT2003-Vita-Builder", console=False)
