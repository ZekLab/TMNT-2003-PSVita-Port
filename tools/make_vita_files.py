#!/usr/bin/env python3
"""Makes the Vita files from your copy of TMNT (PC, 2003): the game code (tmnt_code.so), the game image (image.bin) and the data files.

    python3 tools/make_vita_files.py GAME_FOLDER OUT_FOLDER

GAME_FOLDER is the installed game (or what innoextract wrote); it must hold TMNT_EN.EXE. Copy everything in OUT_FOLDER to
ux0:data/TMNT2003/ on the Vita. Needs Python 3.8+ with pefile and capstone (tools/vita_code/requirements.txt) and LLVM (clang and
ld.lld). Nothing of the game leaves your computer.

    python3 tools/make_vita_files.py --code-only OUT_FOLDER

(developers) only compiles recomp/out/c into OUT_FOLDER/tmnt_code.so.
"""
import concurrent.futures
import glob
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
EXE_SHA1 = "4e56dd020e9b3c9fdf755f3d655fd5f2a84e4777"
# Data files copied to the Vita: name on the disc -> name on the Vita.
DATA_FILES = {"TMNT_EN.DAT": "TMNT.DAT", "TMNT_V00.DAT": "TMNT_V00.DAT", "TMNT_V01.DAT": "TMNT_V01.DAT", "TMNT_V02.DAT": "TMNT_V02.DAT",
              "bgm.afs": "bgm.afs", "se.afs": "se.afs", "voice.afs": "voice.afs"}
LLVM_DIRS = ["/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin", "/usr/local/opt/llvm/bin", "/usr/local/opt/lld/bin",
             "/usr/lib/llvm-18/bin", "/usr/lib/llvm-19/bin", "/usr/lib/llvm-20/bin", r"C:\Program Files\LLVM\bin"]
# The Vita ABI (VitaSDK: Cortex-A9, Thumb-2, hard float, short enums); no unrolling keeps the code as compact as GCC's.
CFLAGS = ["--target=armv7a-none-eabihf", "-mcpu=cortex-a9", "-mfpu=neon", "-mthumb", "-mfloat-abi=hard", "-fshort-enums", "-std=gnu11",
          "-O2", "-fno-strict-aliasing", "-fno-unroll-loops", "-fPIC", "-fvisibility=hidden", "-w", "-nostdinc",
          "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_GPR_LOCAL=1", "-DGUEST_X87_LOCAL=1", "-DGUEST_MEMBASE_LOCAL=1",
          "-DGUEST_FLAGS_LOCAL=1"]


def die(msg):
    print("\nError: " + msg, file=sys.stderr)
    sys.exit(1)


def find_tool(names):
    for name in names:
        exe = name + (".exe" if os.name == "nt" else "")
        found = shutil.which(exe)
        if found:
            return found
        for d in LLVM_DIRS:
            if os.path.isfile(os.path.join(d, exe)):
                return os.path.join(d, exe)
    return None


def find_file(folder, name):
    """Finds `name` (any case) in `folder` or below; the shallowest match wins."""
    best = None
    for dirpath, _, files in os.walk(folder):
        for f in files:
            if f.lower() == name.lower():
                path = os.path.join(dirpath, f)
                if best is None or path.count(os.sep) < best.count(os.sep):
                    best = path
    return best


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def translate(exe):
    for module in ("pefile", "capstone"):
        try:
            __import__(module)
        except ImportError:
            die("the Python package '%s' is missing. Install it with:\n  %s -m pip install -r tools/vita_code/requirements.txt"
                % (module, sys.executable))
    env = dict(os.environ, TMNT_EXE=exe)
    for step in ("discover.py", "gen_c.py", "gen_tables.py"):
        print("Translating: recomp/%s ..." % step, flush=True)
        if subprocess.call([sys.executable, os.path.join(ROOT, "recomp", step)], cwd=ROOT, env=env) != 0:
            die("recomp/%s failed (see the messages above)." % step)


def build_code(out_dir):
    clang, lld = find_tool(["clang"]), find_tool(["ld.lld"])
    if not clang or not lld:
        die("LLVM was not found (clang and ld.lld). Install it:\n  Mac: brew install llvm lld\n  Linux: sudo apt install clang lld\n"
            "  Windows: the LLVM installer from https://github.com/llvm/llvm-project/releases")
    resource = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()
    sources = sorted(glob.glob(os.path.join(ROOT, "recomp", "out", "c", "*.c"))) + [os.path.join(ROOT, "rt", "code_so_entry.c")]
    if len(sources) < 3:
        die("recomp/out/c is empty: the translation did not run.")
    obj_dir = os.path.join(ROOT, "build", "vita_code")
    os.makedirs(obj_dir, exist_ok=True)
    flags = [clang] + CFLAGS + ["-isystem", os.path.join(ROOT, "tools", "vita_code", "include"), "-isystem", os.path.join(resource, "include"),
                                "-I", os.path.join(ROOT, "rt")]

    def compile_one(src):
        obj = os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ".o")
        r = subprocess.run(flags + ["-c", src, "-o", obj], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return src, obj, r.returncode, r.stdout

    print("Compiling the game code (%d files) with %s ..." % (len(sources), clang), flush=True)
    objs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, obj, code, text in pool.map(compile_one, sources):
            if code != 0:
                die("compiling %s failed:\n%s" % (os.path.basename(src), text))
            objs.append(obj)
    so = os.path.join(out_dir, "tmnt_code.so")
    if subprocess.call([lld, "-shared", "-z", "max-page-size=4096", "-z", "now", "-e", "tmnt_code", "-o", so] + objs) != 0:
        die("linking tmnt_code.so failed (see the messages above).")
    print("Wrote %s (%d KB)" % (so, os.path.getsize(so) // 1024))


def main(argv):
    if len(argv) == 2 and argv[0] == "--code-only":
        os.makedirs(argv[1], exist_ok=True)
        build_code(os.path.abspath(argv[1]))
        return
    if len(argv) != 2:
        print(__doc__)
        sys.exit(2)
    game, out = (os.path.abspath(a) for a in argv)
    exe = find_file(game, "TMNT_EN.EXE")
    if not exe:
        die("TMNT_EN.EXE was not found in %s. Give the folder of the installed game." % game)
    if sha1(exe) != EXE_SHA1:
        die("%s is not the supported version (SHA-1 %s).\nOnly the English executable of the English/Russian release works; "
            "the US and European retail discs (SecuROM) do not." % (exe, EXE_SHA1))
    missing = [name for name in DATA_FILES if not find_file(os.path.dirname(exe), name)]
    if missing:
        die("these game files are missing next to TMNT_EN.EXE: " + ", ".join(missing))
    os.makedirs(out, exist_ok=True)

    translate(exe)
    build_code(out)
    shutil.copyfile(os.path.join(ROOT, "recomp", "out", "image.bin"), os.path.join(out, "image.bin"))
    for name, dest in DATA_FILES.items():
        print("Copying %s ..." % name, flush=True)
        shutil.copyfile(find_file(os.path.dirname(exe), name), os.path.join(out, dest))
    print("\nDone. Copy everything in %s to ux0:data/TMNT2003/ on the Vita." % out)


if __name__ == "__main__":
    main(sys.argv[1:])
