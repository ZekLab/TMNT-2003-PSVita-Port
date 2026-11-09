#!/usr/bin/env python3
"""Makes the Vita files from your copy of TMNT (PC, 2003): the game code (tmnt_code.so), the executable and data files, and the
movies converted for the Vita (with tools/movies, when it is there).

    python3 tools/make_vita_files.py GAME_FOLDER OUT_FOLDER

GAME_FOLDER is the installed game (or what innoextract wrote); it must hold TMNT_EN.EXE. Copy everything in OUT_FOLDER to
ux0:data/TMNT2003/ on the Vita. Needs Python 3.8+ with pefile and capstone (tools/vita_code/requirements.txt) and LLVM (clang and
ld.lld) or Zig. Nothing of the game leaves your computer. The graphical builder (tools/builder) runs the same steps.

    python3 tools/make_vita_files.py --code-only OUT_FOLDER

(developers) only compiles recomp/out/c into OUT_FOLDER/tmnt_code.so; flags after the folder go to the compiler.
"""
import concurrent.futures
import contextlib
import glob
import hashlib
import importlib
import os
import shutil
import subprocess
import sys

# In the packaged builder (PyInstaller) the sources travel inside the app.
ROOT = getattr(sys, "_MEIPASS", None) or os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
EXE_SHA1 = "4e56dd020e9b3c9fdf755f3d655fd5f2a84e4777"
VITA_FOLDER = "ux0:data/TMNT2003/"
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
TRANSLATE_STEPS = ("discover", "gen_c", "gen_tables")


class BuildError(Exception):
    pass


def log(msg):
    """Messages; the builder window replaces it."""
    print(msg, file=sys.__stdout__, flush=True)   # not the redirected stdout of the translator steps


def progress(fraction, what):
    """Progress 0..1; the builder window replaces it."""


def die(msg):
    raise BuildError(msg)


def find_zig():
    """A Zig next to the builder (packaged) or given by TMNT_ZIG: its clang and lld compile without any install."""
    exe = "zig.exe" if os.name == "nt" else "zig"
    for path in (os.environ.get("TMNT_ZIG"), os.path.join(ROOT, "zig", exe)):
        if path and os.path.isfile(path):
            return path
    return None


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


def find_compiler():
    """(clang command, ld.lld command, clang's own headers)."""
    zig = find_zig()
    if zig:
        return [zig, "clang"], [zig, "ld.lld"], os.path.join(os.path.dirname(zig), "lib", "include")
    clang, lld = find_tool(["clang"]), find_tool(["ld.lld"])
    if not clang or not lld:
        die("LLVM was not found (clang and ld.lld). Install it:\n  Mac: brew install llvm lld\n  Linux: sudo apt install clang lld\n"
            "  Windows: the LLVM installer from https://github.com/llvm/llvm-project/releases")
    resource = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()
    return [clang], [lld], os.path.join(resource, "include")


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


def check_game(game):
    """TMNT_EN.EXE of the supported version with its data files next to it; returns its path."""
    exe = find_file(game, "TMNT_EN.EXE")
    if not exe:
        die("TMNT_EN.EXE was not found in %s. Give the folder of the installed game." % game)
    if sha1(exe) != EXE_SHA1:
        die("%s is not the supported version (SHA-1 %s).\nOnly the English executable of the English/Russian release works; "
            "the US and European retail discs (SecuROM) do not." % (exe, EXE_SHA1))
    missing = [name for name in DATA_FILES if not find_file(os.path.dirname(exe), name)]
    if missing:
        die("these game files are missing next to TMNT_EN.EXE: " + ", ".join(missing))
    return exe


class _LogWriter:
    """stdout of the translator steps, line by line into log()."""
    def __init__(self):
        self.buf = ""

    def write(self, text):
        self.buf += text
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            log(line)

    def flush(self):
        pass


def translate(exe, out_dir):
    """recomp/discover, gen_c and gen_tables in this process (the packaged builder has no separate Python) into out_dir."""
    for module in ("pefile", "capstone"):
        try:
            __import__(module)
        except ImportError:
            die("the Python package '%s' is missing. Install it with:\n  %s -m pip install -r tools/vita_code/requirements.txt"
                % (module, sys.executable))
    os.environ["TMNT_EXE"] = exe
    os.environ["TMNT_RECOMP_OUT"] = out_dir
    os.makedirs(os.path.join(out_dir, "c"), exist_ok=True)
    recomp = os.path.join(ROOT, "recomp")
    if recomp not in sys.path:
        sys.path.insert(0, recomp)
    for k, step in enumerate(TRANSLATE_STEPS):
        log("Translating: recomp/%s.py ..." % step)
        progress(0.05 + 0.2 * k / len(TRANSLATE_STEPS), "Translating the game code (%d/%d)" % (k + 1, len(TRANSLATE_STEPS)))
        for name in (step, "tmnt_image"):
            sys.modules.pop(name, None)   # OUT and the exe are read at import
        writer = _LogWriter()
        try:
            with contextlib.redirect_stdout(writer):
                code = importlib.import_module(step).main()
        except SystemExit as e:
            code = e.code
        if code not in (None, 0):
            die("recomp/%s.py failed: %s" % (step, code))


def build_code(out_dir, extra_flags=(), src_dir=None, obj_dir=None):
    src_dir = src_dir or os.path.join(ROOT, "recomp", "out", "c")
    obj_dir = obj_dir or os.path.join(ROOT, "build", "vita_code")
    cc, ld, builtin = find_compiler()
    sources = sorted(glob.glob(os.path.join(src_dir, "*.c"))) + [os.path.join(ROOT, "rt", "code_so_entry.c")]
    if len(sources) < 3:
        die("%s is empty: the translation did not run." % src_dir)
    os.makedirs(obj_dir, exist_ok=True)
    flags = cc + CFLAGS + list(extra_flags) + ["-isystem", os.path.join(ROOT, "tools", "vita_code", "include"), "-isystem", builtin,
                                               "-I", os.path.join(ROOT, "rt")]
    nowin = {"creationflags": 0x08000000} if os.name == "nt" else {}   # CREATE_NO_WINDOW: no console flashing per file

    def compile_one(src):
        obj = os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ".o")
        r = subprocess.run(flags + ["-c", src, "-o", obj], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **nowin)
        return src, obj, r.returncode, r.stdout

    log("Compiling the game code (%d files) with %s ..." % (len(sources), " ".join(cc)))
    objs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, obj, code, text in pool.map(compile_one, sources):
            if code != 0:
                die("compiling %s failed:\n%s" % (os.path.basename(src), text))
            objs.append(obj)
            progress(0.25 + 0.45 * len(objs) / len(sources), "Compiling the game code (%d/%d)" % (len(objs), len(sources)))
    so = os.path.join(out_dir, "tmnt_code.so")
    r = subprocess.run(ld + ["-shared", "-z", "max-page-size=4096", "-z", "now", "-e", "tmnt_code", "-o", so] + objs,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **nowin)
    if r.returncode != 0:
        die("linking tmnt_code.so failed:\n" + r.stdout)
    missing = missing_imports(so)
    if missing:
        die("tmnt_code.so needs functions the Vita loader (rt/code_so.c) does not provide: " + ", ".join(missing))
    log("Wrote %s (%d KB)" % (so, os.path.getsize(so) // 1024))


def missing_imports(so):
    """Undefined dynamic symbols of the module that rt/code_so.c's import table lacks (it would refuse the module on the Vita)."""
    import re
    import struct
    d = open(so, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2e)
    sections = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next((h for h in sections if h[1] == 11), None)   # SHT_DYNSYM; its sh_link is the string table
    if not dynsym:
        return []
    strtab = sections[dynsym[6]]
    names = []
    for off in range(dynsym[4], dynsym[4] + dynsym[5], 16):
        name, _, _, _, _, shndx = struct.unpack_from("<IIIBBH", d, off)
        if name and shndx == 0:
            end = d.index(b"\0", strtab[4] + name)
            names.append(d[strtab[4] + name:end].decode())
    table = set(re.findall(r"X\((\w+)\)", open(os.path.join(ROOT, "rt", "code_so.c")).read()))
    return sorted(n for n in names if n not in table)


def copy_data(exe, out):
    """TMNT_EN.EXE (the Vita lays out the game's image from it) and the data files, renamed for the Vita."""
    files = [(exe, "TMNT_EN.EXE")] + [(find_file(os.path.dirname(exe), name), dest) for name, dest in DATA_FILES.items()]
    total = sum(os.path.getsize(src) for src, _ in files) or 1
    done = 0
    for src, dest in files:
        log("Copying %s ..." % os.path.basename(src))
        with open(src, "rb") as fi, open(os.path.join(out, dest), "wb") as fo:
            for block in iter(lambda: fi.read(4 << 20), b""):
                fo.write(block)
                done += len(block)
                progress(0.7 + 0.1 * done / total, "Copying the game files")


def find_movconv():
    """The movie converter next to the builder (packaged), given by TMNT_MOVCONV, or built by hand (README, Movies)."""
    exe = "movconv.exe" if os.name == "nt" else "movconv"
    for path in (os.environ.get("TMNT_MOVCONV"), os.path.join(ROOT, "movconv", exe), os.path.join(ROOT, "build", "movconv", exe)):
        if path and os.path.isfile(path):
            return path
    return None


def movie_count(game_dir):
    """Movies in the AFS archives TMNT_V00..02.DAT (header: "AFS\0", count)."""
    count = 0
    for k in range(3):
        path = find_file(game_dir, "TMNT_V%02d.DAT" % k)
        with open(path, "rb") as f:
            head = f.read(8)
        if head[:3] == b"AFS":
            count += int.from_bytes(head[4:8], "little")
    return count


def convert_movies(game_dir, out):
    """The movies to H.264 for the Vita's hardware decoder (out/movies). Not fatal: without them the Vita plays them choppy."""
    tool = find_movconv()
    if not tool:
        log("Warning: the movie converter is missing, so the movies stay as they are (choppy on the Vita).")
        return
    total = movie_count(game_dir) or 1
    log("Converting the movies for the Vita's video decoder ...")
    progress(0.8, "Converting the movies (0/%d)" % total)
    nowin = {"creationflags": 0x08000000} if os.name == "nt" else {}
    proc = subprocess.Popen([tool, game_dir, os.path.join(out, "movies")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, **nowin)
    done = 0
    for line in proc.stdout:
        log(line.rstrip())
        if line.startswith("V") and ": " in line:   # one line per movie, e.g. "V00_01: 640x448 ..."
            done += 1
            progress(0.8 + 0.2 * done / total, "Converting the movies (%d/%d)" % (done, total))
    if proc.wait() != 0:
        log("Warning: some movies were not converted (see above); those play choppy on the Vita.")


def make_vita_files(game, out, work=None):
    """Everything: check, translate (into work, or recomp/out), compile, copy, convert the movies. Raises BuildError."""
    progress(0.0, "Checking the game")
    exe = check_game(game)
    os.makedirs(out, exist_ok=True)
    recomp_out = os.path.join(work, "recomp") if work else os.path.join(ROOT, "recomp", "out")
    translate(exe, recomp_out)
    build_code(out, src_dir=os.path.join(recomp_out, "c"), obj_dir=os.path.join(work, "obj") if work else None)
    copy_data(exe, out)
    convert_movies(os.path.dirname(exe), out)
    progress(1.0, "Done")
    log("\nDone. Copy everything in %s to %s on the Vita." % (out, VITA_FOLDER))


def main(argv):
    try:
        if len(argv) >= 2 and argv[0] == "--code-only":
            os.makedirs(argv[1], exist_ok=True)
            build_code(os.path.abspath(argv[1]), argv[2:])   # extra compiler flags, e.g. -DGUEST_PROF_EXCL for the profiling build
            return
        if len(argv) != 2:
            print(__doc__)
            sys.exit(2)
        make_vita_files(*(os.path.abspath(a) for a in argv))
    except BuildError as e:
        print("\nError: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv[1:])
