#!/usr/bin/env python3
"""Build DotNetAnywhere. This is the only build script.

Usage:
    python build.py                 # the native runtime build/dna, and build/corlib.dll if mcs is installed
    python build.py --m32           # 32-bit runtime -> build/dna32 (needs gcc-multilib)
    python build.py --debug         # -O0 -g
    python build.py --clean         # remove build/ and the generated header
    python build.py --cc clang      # use another C compiler
    python build.py -j 8            # parallel jobs
    python build.py --no-corlib     # skip build/corlib.dll
    python build.py --run X.exe     # build, then run: build/dna X.exe
    python build.py --wasm          # WebAssembly: build/dna.wasm with clang --target=wasm32-wasi (needs clang, lld, wasi-libc)
    python build.py --lib            # also build/libdna.a, for a native host (native/src/Host.h)
    python build.py --ffi M.json    # the runtime with the C functions of a manifest built in -> build/dna_ffi (see tools/gen_ffi.py)

`make` is a one-line wrapper around `python3 build.py`.

native/src/MetaDataLayout.gen.h, JIT_FusedOps.gen.h and JIT_Fused.gen.h are generated, not committed: this script runs
tools/gen_metadata_layout.py and tools/gen_fused_ops.py to create them when they are missing or older than what they
are generated from.

Objects are cached in build/obj and rebuilt only when the source or any
header is newer than the object.
"""
import argparse
import json
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "native", "src")
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")
GEN = os.path.join(BUILD, "gen")      # (only a --ffi build has generated C of its own: build/gen_ffi)

FFI_INPUTS = []      # the manifest and C files of a --ffi build (what FFI.gen.c depends on)


def sources():
    # NativeHost.c stubs the JavaScript bridge (js-interop.js) for native builds
    return sorted(f for f in os.listdir(SRC) if f.endswith(".c"))


def newest_header():
    return max((os.path.getmtime(os.path.join(SRC, f))
                for f in os.listdir(SRC) if f.endswith(".h")), default=0)


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
FUSED_TOOL = os.path.join(ROOT, "tools", "gen_fused_ops.py")
FUSED_GEN = [os.path.join(SRC, "JIT_FusedOps.gen.h"), os.path.join(SRC, "JIT_Fused.gen.h")]
VSTENCIL_TOOL = os.path.join(ROOT, "tools", "gen_vstencils.py")
VSTENCIL_GEN = [os.path.join(ROOT, "native", "stencils", "vstencils.gen.c"), os.path.join(ROOT, "native", "stencils", "vstencils.json"),
                os.path.join(SRC, "VStencils.gen.h")]
STENCIL_TOOL = os.path.join(ROOT, "tools", "gen_stencils.py")
STENCIL_GEN = [os.path.join(SRC, "Stencils.gen.h")]

# Source files that are generated, not committed: (the script, what it writes, other files it reads)
GENERATED = [
    (LAYOUT_TOOL, [LAYOUT_GEN], [os.path.join(SRC, n) for n in ("MetaDataTables.h", "Types.h", "Compat.h")]),
    (FUSED_TOOL, FUSED_GEN, []),
    (VSTENCIL_TOOL, VSTENCIL_GEN, []),
    (STENCIL_TOOL, STENCIL_GEN, [os.path.join(ROOT, "native", "stencils", "stencils.c")] + VSTENCIL_GEN[:2]),
]


def ensure_generated():
    """Run each generator whose output is missing or older than the generator and the files it reads."""
    for tool, outputs, reads in GENERATED:
        inputs = [tool] + reads
        newest = max(os.path.getmtime(i) for i in inputs if os.path.exists(i))
        if all(os.path.exists(o) and os.path.getmtime(o) >= newest for o in outputs):
            continue
        r = subprocess.run([sys.executable, tool], capture_output=True, text=True)
        if r.returncode != 0:
            print("error: %s failed\n%s" % (os.path.relpath(tool, ROOT), r.stderr or r.stdout), file=sys.stderr)
            return 1
        if "wrote" in r.stdout:
            for o in outputs:
                print("  generated", os.path.relpath(o, ROOT))
    return 0


def main():
    global BUILD, OBJ, GEN
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--m32", action="store_true",
                    help="build 32-bit (-m32); needs gcc-multilib. The default is the native word size")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--corlib", action="store_true",
                    help="compile corlib/*.cs to build/corlib.dll with mcs (the default if mcs is installed; "
                         "this makes a missing mcs an error)")
    ap.add_argument("--no-corlib", action="store_true", help="do not build build/corlib.dll")
    ap.add_argument("--ffi", metavar="MANIFEST.json",
                    help="build the C files and functions of this manifest into the runtime, so that [DllImport] of them is a direct "
                         "call (tools/gen_ffi.py says what the manifest holds). Makes build/dna_ffi, with its own objects")
    ap.add_argument("--lib", action="store_true",
                    help="also make build/libdna.a (build/libdna_ffi.a with --ffi, libdna32.a with --m32): the runtime without dna.c's main, "
                         "for a native program that hosts it (native/src/Host.h)")
    ap.add_argument("--lib-only", action="store_true",
                    help="make only the library (--lib): do not link build/dna[_ffi], which cannot be linked when the --ffi manifest names "
                         "functions that the host program defines")
    ap.add_argument("--build-dir", metavar="DIR",
                    help="build in DIR instead of ./build (objects, generated code, libraries, corlib.dll): a program that hosts DNA keeps its own, "
                         "so building it does not disturb this checkout's build")
    ap.add_argument("--wasm", action="store_true",
                    help="build build/dna.wasm for WebAssembly (clang --target=wasm32-wasi; apt install clang lld wasi-libc libclang-rt-dev-wasm32). "
                         "Run it with: node tools/run_wasm.mjs build/dna.wasm prog.exe (or --run prog.exe)")
    ap.add_argument("--no-wasm-jit", action="store_true",
                    help="with --wasm: leave out the compiler from CIL to wasm (native/src/WasmJIT.c), so that the module needs no host import "
                         "and runs on any WASI runtime. The default build has it, and needs a host that provides dna.emit_wasm (tools/run_wasm.mjs)")
    ap.add_argument("--wasi-sysroot", metavar="DIR", default=os.environ.get("WASI_SYSROOT", "/usr"),
                    help="the wasi-libc sysroot for --wasm (headers in DIR/include/wasm32-wasi, libraries in DIR/lib/wasm32-wasi; default /usr)")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--run", nargs=argparse.REMAINDER,
                    help="after building, run build/dna with these args")
    args = ap.parse_args()
    if args.build_dir:
        BUILD = os.path.abspath(args.build_dir)
        OBJ = os.path.join(BUILD, "obj")
        GEN = os.path.join(BUILD, "gen")

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        for _, outputs, _ in GENERATED:
            for o in outputs:
                if os.path.exists(o):
                    os.remove(o)        # generated; the next build makes them again
        print("cleaned", BUILD)
        return 0

    rc = ensure_generated()
    if rc:
        return rc

    if args.wasm:
        if args.m32:
            print("error: --wasm cannot be combined with --m32", file=sys.stderr)
            return 1
        if args.cc == "gcc":
            args.cc = "clang"        # gcc cannot target wasm
        if not os.path.isdir(os.path.join(args.wasi_sysroot, "include", "wasm32-wasi")):
            print("error: no wasi-libc under %s (apt install wasi-libc libclang-rt-dev-wasm32, or pass --wasi-sysroot)" % args.wasi_sysroot, file=sys.stderr)
            return 1
    if not shutil.which(args.cc):
        print(f"error: compiler '{args.cc}' not found", file=sys.stderr)
        return 1

    if args.m32:
        OBJ = os.path.join(BUILD, "obj32")
    wasm_flags = []
    if args.wasm:
        OBJ = os.path.join(BUILD, "obj_wasm")
        # -nostdlibinc: without it clang also searches the host's /usr/include, which is the wrong libc (no wasm32 ABI)
        wasm_flags = ["--target=wasm32-wasi", "--sysroot=" + args.wasi_sysroot, "-nostdlibinc",
                      "-isystem", os.path.join(args.wasi_sysroot, "include", "wasm32-wasi")]
    if args.ffi:
        # its own objects and generated files: the generated C changes what the runtime is, and the normal build must stay as it is
        OBJ = os.path.join(BUILD, "obj_ffi32" if args.m32 else "obj_ffi_wasm" if args.wasm else "obj_ffi")
        GEN = os.path.join(BUILD, "gen_ffi_wasm" if args.wasm else "gen_ffi")
        os.makedirs(GEN, exist_ok=True)
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import gen_ffi
        import gen_stencils
        manifest, ffi_files, ffi_funcs = gen_ffi.generate(args.ffi, GEN)
        print("ffi: %d function(s) from %s" % (len(ffi_funcs), os.path.relpath(args.ffi, ROOT)))
        # the stencils that call them go in a Stencils.gen.h of their own, which the include path puts before the normal one
        sigs = json.load(open(os.path.join(GEN, "ffi_sigs.json")))
        text = gen_stencils.generate(os.path.join(GEN, "ffi_stencils.gen.c"), sigs["stencil_names"]) if not (args.m32 or args.wasm) else None
        hdr = os.path.join(GEN, "Stencils.gen.h")
        if text is not None and (not os.path.exists(hdr) or open(hdr).read() != text):
            open(hdr, "w").write(text)
        elif text is None and os.path.exists(hdr):
            os.remove(hdr)
        ffi_extra = list(manifest.get("cflags", [])) + sum([["-I", os.path.dirname(p)] for p in ffi_files], [])
        FFI_INPUTS[:] = [os.path.abspath(args.ffi)] + ffi_files
    cflags = ["-std=gnu99", "-Wno-pointer-sign", "-Wno-unused-result",
              "-fno-strict-aliasing", "-I", SRC]
    if args.ffi and os.path.exists(os.path.join(GEN, "Stencils.gen.h")):
        # the FFI build's Stencils.gen.h, with the stencils that call C functions as well. (-I would not do: a quoted include looks in the
        # including file's own directory first, and finds the normal one there)
        cflags = ["-DSTENCILS_HEADER=\"%s\"" % os.path.join(GEN, "Stencils.gen.h")] + cflags
    if args.m32:
        # -msse2 -mfpmath=sse: IEEE single/double arithmetic. The 32-bit default is the x87 FPU, which
        # computes in 80 bits and rounds a second time when storing, so some double divisions come out
        # one ulp from what .NET (SSE2) gives.
        cflags += ["-m32", "-msse2", "-mfpmath=sse"]
        # Debian/Ubuntu multiarch keeps asm/*.h here; -m32 doesn't search it
        if os.path.isdir("/usr/include/x86_64-linux-gnu/asm"):
            cflags += ["-idirafter", "/usr/include/x86_64-linux-gnu"]
    cflags += wasm_flags
    if args.wasm and args.no_wasm_jit:
        cflags += ["-DDNA_NO_WASM_JIT_BUILD"]
    if args.debug:
        cflags += ["-O0", "-g", "-fno-omit-frame-pointer"]
    else:
        cflags += ["-O2", "-DNDEBUG"]

    os.makedirs(OBJ, exist_ok=True)
    hdr_time = newest_header()
    if args.ffi and os.path.exists(os.path.join(GEN, "Stencils.gen.h")):
        hdr_time = max(hdr_time, os.path.getmtime(os.path.join(GEN, "Stencils.gen.h")))

    # Objects are only valid for the flags they were built with: when the
    # compiler or flags change (e.g. --debug), throw the cached objects away.
    stamp = os.path.join(OBJ, ".flags")
    flags_now = " ".join([args.cc] + cflags)
    if os.path.exists(stamp) and open(stamp).read() != flags_now:
        for f in os.listdir(OBJ):
            if f.endswith(".o"):
                os.remove(os.path.join(OBJ, f))
    open(stamp, "w").write(flags_now)

    units = [os.path.join(SRC, n) for n in sources()]
    ffi_extra = ffi_extra if args.ffi else []
    if args.ffi:
        units.append(os.path.join(GEN, "FFI.gen.c"))

    todo = []
    for unit in units:
        base = os.path.basename(unit)
        obj = os.path.join(OBJ, base[:-2] + ".o")
        dep_time = max(os.path.getmtime(unit), hdr_time)
        if args.ffi and base == "FFI.gen.c":
            dep_time = max([dep_time] + [os.path.getmtime(p) for p in FFI_INPUTS])     # the manifest and the C files it includes
        if not os.path.exists(obj) or os.path.getmtime(obj) < dep_time:
            todo.append(unit)

    failed = False
    if todo:
        print(f"compiling {len(todo)} file(s) with {args.cc}...")
        with cf.ThreadPoolExecutor(args.j) as ex:
            for name, cmd, r in ex.map(lambda n: compile_one(args.cc, cflags + (ffi_extra if os.path.basename(n) == "FFI.gen.c" else []), n), todo):
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
    if args.lib or args.lib_only:
        lib = os.path.join(BUILD, "libdna" + ("_ffi" if args.ffi else "") + ("32" if args.m32 else "") + ("_wasm" if args.wasm else "") + ".a")
        if os.path.exists(lib):
            os.remove(lib)
        ar = (shutil.which("llvm-ar") or "llvm-ar") if args.wasm else "ar"
        r = subprocess.run([ar, "rcs", lib] + [o for o in objs if os.path.basename(o) != "dna.o"
                                     # (the weak empty table of FFIDefault.c would satisfy FFI.c, so an archive would never pull in the real one)
                                     and not (args.ffi and os.path.basename(o) == "FFIDefault.o")], capture_output=True, text=True)
        if r.returncode != 0:
            print("ar FAILED")
            print(r.stderr)
            return 1
        print("built", os.path.relpath(lib, ROOT))
    if args.lib_only:
        if args.wasm:
            print("(for a program built with --wasm: link it with %s and these flags: -Wl,-z,stack-size=1048576 -Wl,--export-table -Wl,--growable-table -fuse-ld=lld)"
                  % os.path.relpath(lib, ROOT))
        if args.corlib or (not args.no_corlib and shutil.which("mcs")):
            return build_corlib()
        return 0
    out = os.path.join(BUILD, ("dna_ffi" if args.ffi else "dna") + ("32" if args.m32 else "") + (".wasm" if args.wasm else ""))
    if args.wasm:
        # the default 64 KB wasm stack is too small for the interpreter's C recursion; memory grows on demand
        cmd = [args.cc, "-o", out] + wasm_flags + objs + ["-lm", "-fuse-ld=lld", "-Wl,-z,stack-size=1048576"]
        if not args.no_wasm_jit:
            # the host puts the functions it compiles at run time in the indirect function table, which therefore has to be exported and growable
            cmd += ["-Wl,--export-table", "-Wl,--growable-table"]
    else:
        cmd = [args.cc, "-o", out] + (["-m32"] if args.m32 else []) + objs + ["-lm", "-lpthread"]
    if args.verbose:
        print(" ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("LINK FAILED")
        print(r.stderr)
        return 1
    print("built", os.path.relpath(out, ROOT))
    if args.wasm:
        # build/dna-wasm runs like build/dna (`dna-wasm prog.exe`, any working directory, exit status and environment passed through), so
        # anything that runs the native runtime -- tests/run_tests.py with --wasm, scripts -- can run the wasm one
        launcher = os.path.join(BUILD, "dna-wasm")
        with open(launcher, "w") as f:
            f.write('#!/bin/sh\nexec node --no-warnings "%s" "%s" "$@"\n' % (os.path.join(ROOT, "tools", "run_wasm.mjs"), out))
        os.chmod(launcher, 0o755)
        print("built", os.path.relpath(launcher, ROOT))
    if args.corlib or (not args.no_corlib and shutil.which("mcs")):
        rc = build_corlib()
        if rc:
            return rc
    elif not args.no_corlib:
        print("note: mcs not found, so build/corlib.dll was not built (apt install mono-mcs); "
              "the runtime needs it to run anything")

    if args.run is not None:
        if args.wasm:
            return subprocess.call(["node", "--no-warnings", os.path.join(ROOT, "tools", "run_wasm.mjs"), out] + args.run)
        return subprocess.call([out] + args.run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
