#!/usr/bin/env python3
"""Build DotNetAnywhere. This is the only build script: gcc for a native runtime, emcc for WebAssembly.

Usage:
    python build.py                 # the native runtime build/dna, and build/corlib.dll if mcs is installed
    python build.py --m32           # 32-bit runtime -> build/dna32 (needs gcc-multilib)
    python build.py --wasm          # WebAssembly: build/dna.js and build/dna.wasm (needs Emscripten's emcc)
    python build.py --debug         # -O0 -g
    python build.py --clean         # remove build/ and the generated header
    python build.py --cc clang      # use another C compiler
    python build.py -j 8            # parallel jobs
    python build.py --no-corlib     # skip build/corlib.dll
    python build.py --lower-only    # only run cpprust on native/src/cpp/*.cpp
    python build.py --run X.exe     # build, then run: build/dna X.exe

`make` is a one-line wrapper around `python3 build.py`.

native/src/MetaDataLayout.gen.h is generated, not committed: this script runs tools/gen_metadata_layout.py to
create it when it is missing or older than what it is generated from.

Objects are cached in build/obj and rebuilt only when the source or any
header is newer than the object.

Crust C++ subset modules (native/src/cpp/*.cpp) are first lowered to C with
Crust's tools/cpprust.py into build/gen/, then compiled like any other C file.
Crust is located with --crust DIR, $CRUST_ROOT, or a sibling ../crust checkout.
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "native", "src")
CPP_SRC = os.path.join(SRC, "cpp")
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")
GEN = os.path.join(BUILD, "gen")

def sources():
    # NativeHost.c stubs out what js-interop.js provides under Emscripten, so the --wasm build leaves it out
    return sorted(f for f in os.listdir(SRC) if f.endswith(".c"))


def cpp_sources():
    if not os.path.isdir(CPP_SRC):
        return []
    return sorted(f for f in os.listdir(CPP_SRC) if f.endswith(".cpp"))


def newest_header():
    return max((os.path.getmtime(os.path.join(SRC, f))
                for f in os.listdir(SRC) if f.endswith(".h")), default=0)


def find_crust(explicit):
    for cand in (explicit, os.environ.get("CRUST_ROOT"),
                 os.path.join(ROOT, "..", "crust")):
        if cand and os.path.isfile(os.path.join(cand, "tools", "cpprust.py")):
            return os.path.abspath(cand)
    return None


def lower_cpp(crust, name, force):
    """cpprust: native/src/cpp/X.cpp -> build/gen/X.c. Returns (path, error)."""
    src = os.path.join(CPP_SRC, name)
    out = os.path.join(GEN, name[:-4] + ".c")
    deps = [src] + [os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(".h")]
    if (not force and os.path.exists(out)
            and os.path.getmtime(out) >= max(os.path.getmtime(d) for d in deps)):
        return out, None
    cmd = [sys.executable, os.path.join(crust, "tools", "cpprust.py"), src,
           "-o", out, "--incdir", SRC, "--incdir", CPP_SRC]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        # cpprust writes its diagnostic to the output path on failure
        diag = open(out).read() if os.path.exists(out) else ""
        os.remove(out) if os.path.exists(out) else None
        return None, (diag or r.stderr or r.stdout).strip()
    return out, None


def compile_one(cc, cflags, name):
    # `name` is a bare file name in SRC, or an absolute path (generated C)
    src = name if os.path.isabs(name) else os.path.join(SRC, name)
    base = os.path.basename(name)
    obj = os.path.join(OBJ, base[:-2] + ".o")
    cmd = [cc, "-c", src, "-o", obj] + cflags
    r = subprocess.run(cmd, capture_output=True, text=True)
    return name, cmd, r


def build_corlib():
    """corlib/*.cs -> build/corlib.dll. DNA loads its runtime library by this name."""
    mcs = shutil.which("mcs")
    if not mcs:
        print("error: mcs not found (apt install mono-mcs)", file=sys.stderr)
        return 1
    srcs = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, "corlib"))
                  for f in fs if f.endswith(".cs") and "/obj/" not in dp and "/bin/" not in dp)
    out = os.path.join(BUILD, "corlib.dll")
    if os.path.exists(out) and os.path.getmtime(out) >= max(os.path.getmtime(f) for f in srcs):
        print("build/corlib.dll is up to date")
        return 0
    os.makedirs(BUILD, exist_ok=True)
    r = subprocess.run([mcs, "-nostdlib", "-unsafe", "-target:library", "-nowarn:0219,0414,0649",
                        "-out:" + out] + srcs, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    print("built", os.path.relpath(out, ROOT))
    return 0


LAYOUT_GEN = os.path.join(SRC, "MetaDataLayout.gen.h")
LAYOUT_TOOL = os.path.join(ROOT, "tools", "gen_metadata_layout.py")


def ensure_layout_header():
    """Generate native/src/MetaDataLayout.gen.h if it is missing or older than what it is generated from."""
    inputs = [LAYOUT_TOOL] + [os.path.join(SRC, n) for n in ("MetaDataTables.h", "Types.h", "Compat.h")]
    newest = max(os.path.getmtime(i) for i in inputs if os.path.exists(i))
    if os.path.exists(LAYOUT_GEN) and os.path.getmtime(LAYOUT_GEN) >= newest:
        return 0
    r = subprocess.run([sys.executable, LAYOUT_TOOL], capture_output=True, text=True)
    if r.returncode != 0:
        print("error: could not generate native/src/MetaDataLayout.gen.h\n" + (r.stderr or r.stdout), file=sys.stderr)
        return 1
    if "wrote" in r.stdout:
        print("  generated", os.path.relpath(LAYOUT_GEN, ROOT))
    return 0


def lower_all(crust_dir):
    """Lower every native/src/cpp/*.cpp to C. Returns (generated C files, exit code)."""
    gen = []
    cpps = cpp_sources()
    if cpps:
        crust = find_crust(crust_dir)
        if not crust:
            print("error: Crust not found (needed for native/src/cpp/*.cpp).\n"
                  "  git clone https://github.com/brentharts/crust.git ../crust\n"
                  "  or pass --crust DIR / set CRUST_ROOT",
                  file=sys.stderr)
            return [], 1
        for name in cpps:
            out, err = lower_cpp(crust, name, force=False)
            if err:
                print(f"cpprust REFUSED  cpp/{name}\n{err}", file=sys.stderr)
                return [], 1
            print(f"  cpprust  cpp/{name} -> {os.path.relpath(out, ROOT)}")
            gen.append(out)
    return gen, 0


def build_wasm(gen_units, verbose):
    """WebAssembly with Emscripten: build/dna.js and build/dna.wasm. (This replaced native/build.sh and build.cmd.)"""
    emcc = os.environ.get("EMCC") or shutil.which("emcc")
    if not emcc:
        print("error: emcc not found. Install Emscripten (https://emscripten.org), or set EMCC to its path.",
              file=sys.stderr)
        return 1
    srcs = [os.path.join(SRC, n) for n in sources() if n != "NativeHost.c"] + gen_units
    exports = ["_main", "_JSInterop_CallDotNet", "_Debugger_Continue", "_Debugger_SetBreakPoint",
               "_Debugger_Step", "_Debugger_Reset", "_Debugger_Clear_BreakPoints"]
    cmd = [emcc] + srcs + ["-I", SRC, "-Wno-pointer-sign", "-Oz"]
    for setting in (
            "NO_EXIT_RUNTIME=1",
            "RESERVED_FUNCTION_POINTERS=20",
            "ASSERTIONS=1",
            "EXPORTED_FUNCTIONS=" + repr(exports),
            "EXTRA_EXPORTED_RUNTIME_METHODS=['ccall']",
            "DEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$Browser']",
            "MODULARIZE=1",
            "EXPORT_NAME='DotNetAnywhere'",
            "FORCE_FILESYSTEM=1"):
        cmd += ["-s", setting]
    cmd += ["--js-library", os.path.join(ROOT, "native", "js-interop.js"),
            "-o", os.path.join(BUILD, "dna.js")]
    if verbose:
        print(" ".join(cmd))
    print(f"compiling {len(srcs)} file(s) with emcc...")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("EMCC FAILED")
        print(r.stderr[-3000:])
        return 1
    if r.stderr.strip() and verbose:
        print(r.stderr)
    for f in ("dna.js", "dna.wasm"):
        if os.path.exists(os.path.join(BUILD, f)):
            print("built", os.path.join("build", f))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--m32", action="store_true",
                    help="build 32-bit (-m32); needs gcc-multilib. The default is the native word size")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--crust", help="path to a crust checkout (for cpprust.py)")
    ap.add_argument("--corlib", action="store_true",
                    help="compile corlib/*.cs to build/corlib.dll with mcs (the default if mcs is installed; "
                         "this makes a missing mcs an error)")
    ap.add_argument("--no-corlib", action="store_true", help="do not build build/corlib.dll")
    ap.add_argument("--wasm", action="store_true",
                    help="build for WebAssembly with Emscripten (emcc): build/dna.js and build/dna.wasm")
    ap.add_argument("--lower-only", action="store_true",
                    help="only lower native/src/cpp/*.cpp to build/gen/*.c, then stop")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--run", nargs=argparse.REMAINDER,
                    help="after building, run build/dna with these args")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        if os.path.exists(LAYOUT_GEN):
            os.remove(LAYOUT_GEN)       # generated; the next build makes it again
        print("cleaned", BUILD)
        return 0

    rc = ensure_layout_header()
    if rc:
        return rc

    if args.wasm:
        os.makedirs(BUILD, exist_ok=True)
        os.makedirs(GEN, exist_ok=True)
        gen_units, rc = lower_all(args.crust)
        return rc or build_wasm(gen_units, args.verbose)

    if not shutil.which(args.cc):
        print(f"error: compiler '{args.cc}' not found", file=sys.stderr)
        return 1

    global OBJ
    if args.m32:
        OBJ = os.path.join(BUILD, "obj32")
    cflags = ["-std=gnu99", "-Wno-pointer-sign", "-Wno-unused-result",
              "-fno-strict-aliasing", "-I", SRC]
    if args.m32:
        # -msse2 -mfpmath=sse: IEEE single/double arithmetic. The 32-bit default is the x87 FPU, which
        # computes in 80 bits and rounds a second time when storing, so some double divisions come out
        # one ulp from what .NET (SSE2) gives.
        cflags += ["-m32", "-msse2", "-mfpmath=sse"]
        # Debian/Ubuntu multiarch keeps asm/*.h here; -m32 doesn't search it
        if os.path.isdir("/usr/include/x86_64-linux-gnu/asm"):
            cflags += ["-idirafter", "/usr/include/x86_64-linux-gnu"]
    if args.debug:
        cflags += ["-O0", "-g", "-fno-omit-frame-pointer"]
    else:
        cflags += ["-O2", "-DNDEBUG"]

    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(GEN, exist_ok=True)
    hdr_time = newest_header()

    # Objects are only valid for the flags they were built with: when the
    # compiler or flags change (e.g. --debug), throw the cached objects away.
    stamp = os.path.join(OBJ, ".flags")
    flags_now = " ".join([args.cc] + cflags)
    if os.path.exists(stamp) and open(stamp).read() != flags_now:
        for f in os.listdir(OBJ):
            if f.endswith(".o"):
                os.remove(os.path.join(OBJ, f))
    open(stamp, "w").write(flags_now)

    # Step 1: lower Crust C++ subset modules to C.
    units = [os.path.join(SRC, n) for n in sources()]
    gen_units, rc = lower_all(args.crust)
    if rc:
        return rc
    units += gen_units

    if args.lower_only:
        return 0

    todo = []
    for unit in units:
        base = os.path.basename(unit)
        obj = os.path.join(OBJ, base[:-2] + ".o")
        dep_time = max(os.path.getmtime(unit), hdr_time)
        if not os.path.exists(obj) or os.path.getmtime(obj) < dep_time:
            todo.append(unit)

    failed = False
    if todo:
        print(f"compiling {len(todo)} file(s) with {args.cc}...")
        with cf.ThreadPoolExecutor(args.j) as ex:
            for name, cmd, r in ex.map(lambda n: compile_one(args.cc, cflags, n), todo):
                name = os.path.basename(name)
                if args.verbose:
                    print(" ".join(cmd))
                if r.returncode != 0:
                    failed = True
                    print(f"FAILED  {name}")
                    print("\n".join(r.stderr.splitlines()[:15]))
                else:
                    print(f"  ok    {name}")
                    if r.stderr and args.verbose:
                        print(r.stderr)
    else:
        print("objects up to date")

    if failed:
        return 1

    objs = [os.path.join(OBJ, os.path.basename(u)[:-2] + ".o") for u in units]
    out = os.path.join(BUILD, "dna32" if args.m32 else "dna")
    cmd = [args.cc, "-o", out] + (["-m32"] if args.m32 else []) + objs + ["-lm", "-lpthread"]
    if args.verbose:
        print(" ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("LINK FAILED")
        print(r.stderr)
        return 1
    print("built", os.path.relpath(out, ROOT))
    if args.corlib or (not args.no_corlib and shutil.which("mcs")):
        rc = build_corlib()
        if rc:
            return rc
    elif not args.no_corlib:
        print("note: mcs not found, so build/corlib.dll was not built (apt install mono-mcs); "
              "the runtime needs it to run anything")

    if args.run is not None:
        return subprocess.call([out] + args.run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
