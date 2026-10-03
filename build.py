#!/usr/bin/env python3
"""Build DotNetAnywhere (native/src/*.c) with gcc.

Usage:
    python build.py --m32 --corlib  # what you want: 32-bit runtime build/dna32 + build/corlib.dll
    python build.py                 # release build  -> build/dna (64-bit: compiles, cannot run .NET code)
    python build.py --debug         # -O0 -g
    python build.py --clean         # remove build/
    python build.py --cc clang      # use another compiler
    python build.py --m32           # 32-bit build -> build/dna32 (DNA targets 32-bit)
    python build.py -j 8            # parallel jobs
    python build.py --corlib        # also build build/corlib.dll from corlib/ (needs mcs)
    python build.py --lower-only    # only run cpprust (used by native/build.sh for emcc)
    python build.py --run X.exe     # build, then run: build/dna X.exe

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
    # NativeHost.c stubs out what js-interop.js provides under Emscripten (native/build.sh leaves it out)
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
    r = subprocess.run([mcs, "-nostdlib", "-unsafe", "-target:library", "-nowarn:0219,0414,0649",
                        "-out:" + out] + srcs, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    print("built", os.path.relpath(out, ROOT))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--m32", action="store_true",
                    help="build 32-bit (-m32); needs gcc-multilib. DNA was written for 32-bit pointers")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--crust", help="path to a crust checkout (for cpprust.py)")
    ap.add_argument("--corlib", action="store_true",
                    help="also compile corlib/*.cs to build/corlib.dll with mcs")
    ap.add_argument("--lower-only", action="store_true",
                    help="only lower native/src/cpp/*.cpp to build/gen/*.c, then stop")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--run", nargs=argparse.REMAINDER,
                    help="after building, run build/dna with these args")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        print("cleaned", BUILD)
        return 0

    if not shutil.which(args.cc):
        print(f"error: compiler '{args.cc}' not found", file=sys.stderr)
        return 1

    global OBJ
    if args.m32:
        OBJ = os.path.join(BUILD, "obj32")
    cflags = ["-std=gnu99", "-Wno-pointer-sign", "-Wno-unused-result",
              "-fno-strict-aliasing", "-I", SRC]
    if args.m32:
        cflags += ["-m32"]
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
    cpps = cpp_sources()
    if cpps:
        crust = find_crust(args.crust)
        if not crust:
            print("error: Crust not found (needed for native/src/cpp/*.cpp).\n"
                  "  git clone https://github.com/brentharts/crust.git ../crust\n"
                  "  or pass --crust DIR / set CRUST_ROOT",
                  file=sys.stderr)
            return 1
        for name in cpps:
            out, err = lower_cpp(crust, name, force=False)
            if err:
                print(f"cpprust REFUSED  cpp/{name}\n{err}", file=sys.stderr)
                return 1
            print(f"  cpprust  cpp/{name} -> {os.path.relpath(out, ROOT)}")
            units.append(out)

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
    if not args.m32:
        print("note: DNA assumes 32-bit pointers. This 64-bit build compiles but cannot run .NET\n"
              "      programs (it fails loading corlib); use --m32 (needs gcc-multilib) to run code.")

    if args.corlib:
        rc = build_corlib()
        if rc:
            return rc

    if args.run is not None:
        return subprocess.call([out] + args.run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
