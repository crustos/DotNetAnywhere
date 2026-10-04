#!/usr/bin/env python3
"""Run DotNetAnywhere tests. Requires a prior `python build.py`."""
import glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
# The runtime under test. `--64` tests build/dna, `--32` tests build/dna32, DNA_BIN=path names one; with none of
# those it is build/dna if that exists, else build/dna32. The tests compare with Mono / .NET, so they do not depend
# on the pointer size.
for _flag, _name in (("--64", "dna"), ("--32", "dna32")):
    if _flag in sys.argv:
        sys.argv.remove(_flag)
        os.environ["DNA_BIN"] = os.path.join(BUILD, _name)
if "DNA_BIN" not in os.environ:
    os.environ["DNA_BIN"] = os.path.join(BUILD, "dna" if os.path.exists(os.path.join(BUILD, "dna")) else "dna32")
DNA_BIN = os.environ["DNA_BIN"]

# Every external run gets a timeout, so one hanging program (a runtime that spins) fails its own check
# instead of stalling the whole suite. DNA_TEST_TIMEOUT overrides the 60 seconds.
_real_run = subprocess.run
def _run_with_timeout(*a, **kw):
    kw.setdefault("timeout", int(os.environ.get("DNA_TEST_TIMEOUT", "60")))
    try:
        return _real_run(*a, **kw)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return subprocess.CompletedProcess(a[0], -999, stdout=out, stderr="TIMEOUT after %ss" % kw["timeout"])
subprocess.run = _run_with_timeout
SRC = os.path.join(ROOT, "native", "src")


def heaptree_difftest():
    exe = os.path.join(BUILD, "heaptree_difftest")
    lowered = os.path.join(BUILD, "gen", "HeapTree.c")
    cmd = ["gcc", "-std=gnu99", "-O1", "-g", "-fsanitize=address,undefined",
           "-I", SRC, os.path.join(ROOT, "tests", "heaptree_difftest.c"),
           lowered, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr); return False
    ok = True
    for seed in (1, 2, 3, 42, 1234):
        r = subprocess.run([exe, str(seed), "60000"], capture_output=True, text=True)
        print(" ", r.stdout.strip() or r.stderr.strip())
        ok &= r.returncode == 0
    return ok


def metadata_layout_current():
    """native/src/MetaDataLayout.gen.h is generated (by build.py) from tools/gen_metadata_layout.py and the row
    structs; it must be what they produce now."""
    # --verify-with-compiler also cross-checks the layout the generator computes against gcc -m32's, when there is one
    for gen in ("gen_fused_ops.py", "gen_stencils.py"):
        fr = subprocess.run([sys.executable, os.path.join(ROOT, "tools", gen), "--check"], capture_output=True, text=True)
        if fr.returncode:
            print("  FAIL", (fr.stdout + fr.stderr).strip().splitlines()[-1])
            return False
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_metadata_layout.py"), "--check",
                        "--verify-with-compiler"], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip().splitlines()
    cross = [l for l in out if l.startswith("layout cross-check") or "cross-check skipped" in l]
    print("  %s MetaDataLayout.gen.h is up to date%s" % ("ok  " if r.returncode == 0 else "FAIL",
          ("; " + cross[0]) if cross and r.returncode == 0 else ""))
    if r.returncode:
        for l in out[-4:]:
            print("   ", l)
    return r.returncode == 0


def internalcall_params_ok():
    """Every native method reads its arguments at their real offsets, for 32- and 64-bit pointers
    (tools/check_internalcall_params.py compares each read with the native's registered signature)."""
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_internalcall_params.py")],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip().splitlines()
    print("  %s %s" % ("ok  " if r.returncode == 0 else "FAIL", out[-1] if out else "native parameter reads"))
    for l in out[:-1]:
        print("   ", l)
    return r.returncode == 0


def jit_uninitialised_locals():
    """Deterministic guard for the JIT.c bug where `goto cilCallVirtConstrained`
    skipped the initialiser of `dynamicallyBoxReturnValue`. The crash it caused
    depends on stack garbage; gcc's warning for it does not."""
    r = subprocess.run(["gcc", "-c", "-O2", "-Wmaybe-uninitialized", "-Wno-pointer-sign",
                        "-I", SRC, os.path.join(SRC, "JIT.c"), "-o", os.devnull],
                       capture_output=True, text=True)
    bad = [l for l in r.stderr.splitlines() if "dynamicallyBoxReturnValue" in l]
    for l in bad:
        print(" ", l)
    print("  ok   no maybe-uninitialized warning for dynamicallyBoxReturnValue" if not bad else "")
    return not bad


def net8_run(cs, out):
    """Compile `cs` with Roslyn against the real .NET reference assemblies and run it on the .NET runtime.
    Returns its stdout, or None if no .NET SDK/runtime is installed (the caller then skips)."""
    import shutil
    dotnet = shutil.which("dotnet")
    csc = (glob.glob("/usr/lib/dotnet/sdk/*/Roslyn/bincore/csc.dll") or glob.glob("/usr/share/dotnet/sdk/*/Roslyn/bincore/csc.dll") or [None])[0]
    refs = (glob.glob("/usr/lib/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net*") or glob.glob("/usr/share/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net*") or [None])[0]
    rts = sorted(glob.glob("/usr/lib/dotnet/shared/Microsoft.NETCore.App/*") + glob.glob("/usr/share/dotnet/shared/Microsoft.NETCore.App/*"))
    if not (dotnet and csc and refs and rts):
        return None
    dll = os.path.join(out, os.path.basename(cs)[:-3] + ".net.dll")
    cmd = ["dotnet", csc, "-noconfig", "-nologo", "-unsafe", "-nullable:disable", "-target:exe", "-out:" + dll, cs]
    cmd += ["-r:" + f for f in glob.glob(os.path.join(refs, "*.dll"))]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print("   .NET compile failed:", (r.stdout + r.stderr)[:200]); return ""
    rt = rts[-1]
    open(dll[:-4] + ".runtimeconfig.json", "w").write(
        '{"runtimeOptions":{"tfm":"net8.0","framework":{"name":"Microsoft.NETCore.App","version":"%s"}}}' % os.path.basename(rt))
    r = subprocess.run(["dotnet", "exec", "--runtimeconfig", dll[:-4] + ".runtimeconfig.json", dll], capture_output=True, text=True)
    return r.stdout


# The results of these depend on the C library underneath: a 32-bit process (dna32) calls the i386 libm and
# .NET on x86-64 calls the x86-64 one, and those return different bits for these functions. For the 32-bit
# build they are reported by the MathBits test but not asserted. (Everything else must agree bit for bit, and
# the 64-bit build must match on all of them.)
LIBM_DEPENDENT = {"MathF.Tan", "MathF.Atan", "MathF.Sinh", "MathF.Tanh", "MathF.Log10", "MathF.Cbrt", "MathF.Atan2",
                  "Math.Sinh", "Math.Cosh", "Math.Tanh", "Math.Exp", "Math.Log10", "Math.Cbrt", "Math.Atan2", "Math.Pow"}


def dotnet_programs():
    """Compile tests/dotnet/*.cs against corlib.dll and run them on the runtime under test (build/dna32 or build/dna)."""
    import shutil
    dna = DNA_BIN
    if not shutil.which("mcs") or not os.path.exists(dna) \
            or not os.path.exists(os.path.join(BUILD, "corlib.dll")):
        print("  SKIP (needs: mcs, %s and build/corlib.dll)" % os.path.basename(DNA_BIN))
        return True
    expected = {"GcStress": "live nodes: 5000\nchecksum: 91323100"}
    # Programs whose expectations must also hold on the reference runtime. Run under Mono
    # too, so a wrong expectation is caught there and not baked into DNA's behaviour.
    # (MarshalRefusal is deliberately DNA-only: the reference Marshal accepts strings.)
    parity = {"ManyLocalsAndArgs", "MarshalLayout", "ExceptionUnwind", "DictionaryConstrained", "ListBounds", "NullFields",
              "Int64AndFloatOps", "RefAccess", "ConversionChains",
              "DelegateVirtual", "TypedReferences"}
    # Generated programs that print one line per case: DNA's whole stdout must equal Mono's.
    output_parity = {"CheckedConversions", "UncheckedConversions", "ArithmeticMatrix", "UnboxChecks", "EnumFormatting", "ValueTypeBases", "ExceptionFilters", "Stopwatch", "StaticFields", "TryRegions", "DictionaryOps", "StructArrays", "FusedOps", "StencilBlocks", "StencilFields", "StencilLoops", "ThreadSleep", "ArrayBounds", "FloatCompare", "StencilFloat", "StencilArrays", "StencilWide", "StencilInline", "StencilFields8", "CallPaths", "DelegatePaths", "GcRoots", "StencilIslands", "StencilInlineCold", "StencilRegisters"}
    out = os.path.join(BUILD, "dotnet"); os.makedirs(out, exist_ok=True)
    shutil.copy(os.path.join(BUILD, "corlib.dll"), out)
    ok = True
    for cs in sorted(glob.glob(os.path.join(ROOT, "tests", "dotnet", "*.cs"))):
        name = os.path.basename(cs)[:-3]
        if name.startswith("Ffi"):
            continue        # needs the runtime built with an FFI manifest and the C library: ffi_calls() below
        dna_only_cs = name.endswith(".dnaonly")   # self-checking; the reference runtime cannot run it
        exe = os.path.join(out, name + ".exe")
        r = subprocess.run(["mcs", "-nostdlib", "-unsafe", "-r:" + os.path.join(out, "corlib.dll"),
                            "-nowarn:0219", "-out:" + exe, cs], capture_output=True, text=True)
        if r.returncode:
            print("  compile failed:", name, r.stdout + r.stderr); ok = False; continue
        ref_out = None
        if name == "MathBits":
            ref = net8_run(cs, out)
            if ref is None:
                print("  SKIP MathBits (needs a .NET SDK for the reference run)")
                continue
            d = subprocess.run([dna, exe], cwd=out, capture_output=True, text=True)
            key = lambda t: dict((l.rsplit(" ", 1)[0], l.rsplit(" ", 1)[1].strip()) for l in t.splitlines() if " " in l and not l.startswith("Total execution time"))
            a, b = key(ref), key(d.stdout)
            # The libm-dependent results are only excused for the 32-bit build (which calls the i386 C library);
            # a 64-bit build calls the same x86-64 one .NET does, so there every result must match.
            excused = LIBM_DEPENDENT if os.path.basename(dna) == "dna32" else set()
            bad = [k for k in a if a[k] != b.get(k) and k not in excused]
            soft = [k for k in a if a[k] != b.get(k) and k in excused]
            good = d.returncode == 0 and not bad and len(b) == len(a) and len(a) > 0
            print("  %s MathBits (%d of %d results bit-identical to .NET%s)" % (
                "ok  " if good else "FAIL", len(a) - len(bad) - len(soft), len(a),
                "; %d libm-dependent not asserted" % len(soft) if soft else ""))
            if bad:
                print("    differ:", ", ".join(bad)); ok = False
            elif not good:
                ok = False
            continue
        if dna_only_cs:
            d = subprocess.run([dna, exe], cwd=out, capture_output=True, text=True)
            print("  %s %s (DNA only, rc=%d, expected 0)" % ("ok  " if d.returncode == 0 else "FAIL", name, d.returncode))
            if d.returncode != 0:
                print("   ", "\n".join(l for l in d.stdout.splitlines() if l.strip())[-200:]); ok = False
            continue
        if (name in parity or name in output_parity) and shutil.which("mono"):
            ref = os.path.join(out, name + ".ref.exe")
            r = subprocess.run(["mcs", "-unsafe", "-nowarn:0219", "-out:" + ref, cs], capture_output=True, text=True)
            rr = subprocess.run(["mono", ref], capture_output=True, text=True) if r.returncode == 0 else r
            if rr.returncode != 0:
                print("  FAIL %s: the reference runtime (Mono) disagrees, rc=%s %s" % (name, rr.returncode, rr.stdout[:80]))
                ok = False
                continue
            ref_out = "\n".join(l.rstrip() for l in rr.stdout.splitlines() if l.strip())
        r = subprocess.run([dna, exe], cwd=out, capture_output=True, text=True)
        got = "\n".join(l.rstrip() for l in r.stdout.splitlines()
                        if l.strip() and not l.startswith("Total execution time"))
        if name in output_parity:
            if ref_out is None:
                print("  SKIP %s (needs mono)" % name)
                continue
            good = r.returncode == 0 and got == ref_out
            if not good:
                a, b = got.splitlines(), ref_out.splitlines()
                first = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                print("  FAIL %s: %d lines vs Mono's %d; first difference at line %d: DNA %r, Mono %r" % (
                    name, len(a), len(b), first + 1, a[first] if first < len(a) else None,
                    b[first] if first < len(b) else None))
                ok = False
                continue
            print("  ok   %s (%d lines identical to Mono)" % (name, len(got.splitlines())))
            continue
        good = r.returncode == 0 and got == expected.get(name, got)
        print("  %s %s" % ("ok  " if good else "FAIL", name))
        if not good:
            print(got, r.stderr); ok = False
    return ok


def il_programs():
    """tests/il/*.il: IL for opcodes C# does not emit. `@ASM@` stands for the core library
    (mscorlib for Mono, corlib for DNA). Assembled once for each; the exit code and the output
    must be identical."""
    import shutil
    dna = DNA_BIN
    if not (shutil.which("ilasm") and shutil.which("mono")) or not os.path.exists(dna) \
            or not os.path.exists(os.path.join(BUILD, "corlib.dll")):
        print("  SKIP (needs: ilasm, mono, `python build.py --m32 --corlib`)")
        return True
    out = os.path.join(BUILD, "il"); os.makedirs(out, exist_ok=True)
    shutil.copy(os.path.join(BUILD, "corlib.dll"), out)
    ok = True
    for il in sorted(glob.glob(os.path.join(ROOT, "tests", "il", "*.il"))):
        name = os.path.basename(il)[:-3]
        text = open(il).read()
        exes = {}
        # *.dnaonly.il: the reference runtime cannot run it (it aborts), so DNA alone must exit 0
        dna_only = name.endswith(".dnaonly")
        # *.dnafail.il: invalid IL that DNA must refuse. Its first line is `// EXPECT-CRASH: <text>`;
        # DNA must exit non-zero and say <text>. (DNA has no catchable InvalidProgramException.)
        dna_fail = name.endswith(".dnafail")
        if dna_fail:
            dna_only = True
        for tag, asm in ((("dna", "corlib"),) if dna_only else (("mono", "mscorlib"), ("dna", "corlib"))):
            src = os.path.join(out, "%s_%s.il" % (name, tag))
            open(src, "w").write(text.replace("@ASM@", asm))
            exe = os.path.join(out, "%s_%s.exe" % (name, tag))
            r = subprocess.run(["ilasm", src, "/output:" + exe], capture_output=True, text=True)
            if r.returncode or not os.path.exists(exe):
                print("  FAIL %s: ilasm (%s): %s" % (name, tag, (r.stdout + r.stderr)[-300:])); ok = False; break
            exes[tag] = exe
        else:
            if dna_fail:
                want = text.splitlines()[0].split("EXPECT-CRASH:", 1)[1].strip()
                d = subprocess.run([dna, exes["dna"]], capture_output=True, text=True, cwd=out)
                good = d.returncode != 0 and want in d.stdout
                print("  %s %s (DNA rc=%d, %s)" % ("ok  " if good else "FAIL", name, d.returncode,
                      "refused with the expected message" if good else "expected: " + want))
                if not good:
                    print("    DNA said:", d.stdout.strip()[-200:])
                ok &= good
                continue
            if dna_only:
                d = subprocess.run([dna, exes["dna"]], capture_output=True, text=True, cwd=out)
                print("  %s %s (DNA rc=%d, expected 0)" % ("ok  " if d.returncode == 0 else "FAIL", name, d.returncode))
                ok &= d.returncode == 0
                continue
            m = subprocess.run(["mono", exes["mono"]], capture_output=True, text=True, cwd=out)
            d = subprocess.run([dna, exes["dna"]], capture_output=True, text=True, cwd=out)
            strip = lambda t: "\n".join(l.rstrip() for l in t.splitlines()
                                       if l.strip() and not l.startswith("Total execution time"))
            good = m.returncode == d.returncode and strip(m.stdout) == strip(d.stdout)
            print("  %s %s (Mono rc=%d, DNA rc=%d)" % ("ok  " if good else "FAIL", name, m.returncode, d.returncode))
            if not good:
                print("    DNA:", strip(d.stdout)[-300:] or strip(d.stderr)[-300:]); ok = False
    return ok


def stencil_coverage():
    """Every stencil must be compiled by at least one of the stencil tests. A native block that is never made gives the right
    answer, only slower, so no comparison with Mono can notice: (it is how every ldelem.r4 went unrecognised for a while, because
    its handler's address was not the one that was compared). Counted with DNA_STENCIL_STATS=1."""
    import re
    if os.path.basename(DNA_BIN) != "dna" or os.environ.get("DNA_NO_STENCILS") or os.environ.get("DNA_NO_FUSION") or os.environ.get("DNA_NO_INLINE") \
            or os.environ.get("DNA_NO_VSTACK"):
        print("  SKIP (native blocks are only used by build/dna, and not with DNA_NO_STENCILS / DNA_NO_FUSION / DNA_NO_VSTACK; some stencils exist only for inlining, DNA_NO_INLINE)")
        return True
    header = os.path.join(ROOT, "native", "src", "Stencils.gen.h")
    text = open(header).read() if os.path.exists(header) else ""
    m = re.search(r"stencilNames\[ST_COUNT\] = \{([^}]*)\}", text)
    if not m:
        print("  SKIP (no stencils on this host)")
        return True
    names = re.findall(r'"(\w+)"', m.group(1))
    out = os.path.join(BUILD, "dotnet")
    used = dict((n, 0) for n in names)
    ran = 0
    for exe in sorted(glob.glob(os.path.join(out, "*.exe"))):
        base = os.path.basename(exe)[:-4]
        if not (base.startswith("Stencil") or base in ("ArrayBounds", "FloatCompare", "FusedOps")):
            continue
        ran += 1
        # twice: the register pass (the default) takes the place of the plain stencils where it can, and DNA_NO_VSTACK=1 reaches them
        # (they are also what it falls back to for what it does not do)
        for extra in ({}, {"DNA_NO_VSTACK": "1"}):
            env = dict(os.environ, DNA_STENCIL_STATS="1", **extra)
            r = _run_with_timeout([DNA_BIN, exe], cwd=out, capture_output=True, text=True, env=env)
            for line in r.stderr.splitlines():
                mm = re.match(r"\s+(\w+)\s+(\d+)$", line)
                if mm and mm.group(1) in used:
                    used[mm.group(1)] += int(mm.group(2))
    # (without islands a result that goes to a call is not in a block, so a few shapes of register stencil do not occur; those names start with v)
    never = [n for n in names if used[n] == 0 and not (os.environ.get("DNA_NO_ISLANDS") and n.startswith("v"))]
    if ran == 0:
        print("  SKIP (the stencil tests were not built)")
        return True
    if never:
        print("  FAIL: these stencils were never compiled by any stencil test: " + ", ".join(never))
        return False
    print("  ok   all %d stencils are exercised (%d programs)" % (len(names), ran))
    return True


def register_tests_in_sync():
    """tests/dotnet/StencilRegisters.cs is generated from the table of register stencils; a stencil added without regenerating it would not be tested"""
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_vstencil_tests.py"), "--check"], capture_output=True, text=True)
    print("  %s %s" % ("ok  " if r.returncode == 0 else "FAIL", r.stdout.strip() or r.stderr.strip()[-200:]))
    return r.returncode == 0


def inlining_happens():
    """StencilInline and StencilInlineCold compare equal with and without inlining, which says nothing about whether any call was inlined: a
    call that is not is only slower. So count them (DNA_STENCIL_STATS=1 prints how many calls were put into blocks), and check that
    DNA_NO_INLINE=1 turns it off."""
    import re
    if os.path.basename(DNA_BIN) != "dna" or os.environ.get("DNA_NO_STENCILS") or os.environ.get("DNA_NO_FUSION") or os.environ.get("DNA_NO_INLINE"):
        print("  SKIP (native blocks are only used by build/dna, and not with DNA_NO_STENCILS / DNA_NO_FUSION / DNA_NO_INLINE)")
        return True
    out = os.path.join(BUILD, "dotnet")
    ok = True
    # (what has a cold path can only be inlined with islands, so with DNA_NO_ISLANDS=1 far fewer are)
    no_islands = bool(os.environ.get("DNA_NO_ISLANDS"))
    for name, least in (("StencilInline", 60 if not no_islands else 40), ("StencilInlineCold", 150 if not no_islands else 30)):
        exe = os.path.join(out, name + ".exe")
        if not os.path.exists(exe):
            print("  SKIP (%s was not built)" % name)
            continue
        def count(extra):
            env = dict(os.environ, DNA_STENCIL_STATS="1", **extra)
            r = _run_with_timeout([DNA_BIN, exe], cwd=out, capture_output=True, text=True, env=env)
            m = re.search(r"calls inlined into blocks: (\d+)", r.stderr)
            return int(m.group(1)) if m else None
        on, off = count({}), count({"DNA_NO_INLINE": "1"})
        if on is None:
            print("  SKIP (no native blocks on this host)")
            return True
        if on < least or off != 0:
            print("  FAIL: %s: %s calls inlined by default (expected at least %d), %s with DNA_NO_INLINE=1 (expected 0)" % (name, on, least, off))
            ok = False
        else:
            print("  ok   %s: %d calls inlined into blocks, 0 with DNA_NO_INLINE=1" % (name, on))
    return ok


def islands_happen():
    """StencilIslands compares equal with and without islands, which says nothing about whether any instruction was run as an island (one
    that is not is only slower), so count them with DNA_STENCIL_STATS=1 and check that DNA_NO_ISLANDS=1 turns them off."""
    import re
    if os.path.basename(DNA_BIN) != "dna" or os.environ.get("DNA_NO_STENCILS") or os.environ.get("DNA_NO_FUSION") or os.environ.get("DNA_NO_ISLANDS"):
        print("  SKIP (native blocks are only used by build/dna, and not with DNA_NO_STENCILS / DNA_NO_FUSION / DNA_NO_ISLANDS)")
        return True
    out = os.path.join(BUILD, "dotnet")
    exe = os.path.join(out, "StencilIslands.exe")
    if not os.path.exists(exe):
        print("  SKIP (StencilIslands was not built)")
        return True
    def count(extra):
        env = dict(os.environ, DNA_STENCIL_STATS="1", **extra)
        r = _run_with_timeout([DNA_BIN, exe], cwd=out, capture_output=True, text=True, env=env)
        m = re.search(r"islands in blocks: (\d+)", r.stderr)
        return int(m.group(1)) if m else None
    on, off = count({}), count({"DNA_NO_ISLANDS": "1"})
    if on is None:
        print("  SKIP (no native blocks on this host)")
        return True
    if on < 20 or off != 0:
        print("  FAIL: %s islands by default (expected at least 20), %s with DNA_NO_ISLANDS=1 (expected 0)" % (on, off))
        return False
    print("  ok   %d islands in blocks in StencilIslands, 0 with DNA_NO_ISLANDS=1" % on)
    return True


def ffi_calls():
    """[DllImport] of C functions named in a manifest (build.py --ffi tests/ffi/mylib.json). Builds that runtime (build/dna_ffi, or dna_ffi32
    for the 32-bit run) and the same C as a shared library for Mono, and compares FfiCalls with Mono's output (in the default mode and
    with DNA_NO_STENCILS=1). On the 64-bit build it also checks that the generated stencils were used (a call that is not one is only
    slower), and that a DllImport that disagrees with the manifest is refused when it is compiled."""
    import re, shutil
    m32 = os.path.basename(DNA_BIN) == "dna32"
    if not (shutil.which("mono") and shutil.which("gcc") and shutil.which("mcs")) or not os.path.exists(os.path.join(BUILD, "corlib.dll")):
        print("  SKIP (needs: mono, mcs, gcc and build/corlib.dll)")
        return True
    manifest = os.path.join(ROOT, "tests", "ffi", "mylib.json")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "build.py"), "--ffi", manifest, "--no-corlib"] + (["--m32"] if m32 else []),
                       capture_output=True, text=True)
    binary = os.path.join(BUILD, "dna_ffi32" if m32 else "dna_ffi")
    if r.returncode != 0 or not os.path.exists(binary):
        print("  FAIL: the runtime with the FFI manifest did not build:\n" + (r.stdout + r.stderr)[-600:])
        return False
    out = os.path.join(BUILD, "ffi"); os.makedirs(out, exist_ok=True)
    shutil.copy(os.path.join(BUILD, "corlib.dll"), out)
    lib = subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-o", os.path.join(out, "libmylib.so"), os.path.join(ROOT, "tests", "ffi", "mylib.c")],
                         capture_output=True, text=True)
    if lib.returncode:
        print("  FAIL: libmylib.so did not build:", lib.stderr[:300]); return False
    ok = True
    clean = lambda t: "\n".join(l.rstrip() for l in t.splitlines() if l.strip() and not l.startswith("Total execution time"))
    for name, least_stencils in (("FfiCalls", 12), ("FfiMarshal", 4)):
        cs = os.path.join(ROOT, "tests", "dotnet", name + ".cs")
        ref, exe = os.path.join(out, name + ".ref.exe"), os.path.join(out, name + ".exe")
        a = subprocess.run(["mcs", "-unsafe", "-out:" + ref, cs], capture_output=True, text=True)
        b = subprocess.run(["mcs", "-nostdlib", "-unsafe", "-r:" + os.path.join(out, "corlib.dll"), "-out:" + exe, cs], capture_output=True, text=True)
        if a.returncode or b.returncode:
            print("  FAIL: %s did not compile:" % name, (a.stdout + a.stderr + b.stdout + b.stderr)[:300]); ok = False; continue
        want = clean(subprocess.run(["mono", ref], capture_output=True, text=True, env=dict(os.environ, LD_LIBRARY_PATH=out)).stdout)
        if want == "":
            print("  FAIL: Mono printed nothing for %s" % name); ok = False; continue
        for label, env in (("default", {}), ("DNA_NO_STENCILS=1", {"DNA_NO_STENCILS": "1"})):
            if m32 and label != "default":
                continue
            d = subprocess.run([binary, exe], cwd=out, capture_output=True, text=True, env=dict(os.environ, **env))
            if d.returncode != 0 or clean(d.stdout) != want:
                print("  FAIL %s [%s]: %s" % (name, label, (d.stdout + d.stderr)[-200:])); ok = False
            else:
                print("  ok   %s [%s] (%d lines identical to Mono)" % (name, label, len(want.splitlines())))
        if not m32 and not os.environ.get("DNA_NO_STENCILS") and not os.environ.get("DNA_NO_FUSION"):
            d = subprocess.run([binary, exe], cwd=out, capture_output=True, text=True, env=dict(os.environ, DNA_STENCIL_STATS="1"))
            used = sorted(set(n for n, c in re.findall(r"^\s*(ffi_\w+)\s+(\d+)", d.stderr, re.M) if int(c) > 0))
            least = max(2, least_stencils // 2) if os.environ.get("DNA_NO_ISLANDS") else least_stencils    # (without islands a call among other calls is often not in a block)
            if len(used) < least:
                print("  FAIL: %s used only %d of the generated call stencils (expected at least %d): %s" % (name, len(used), least, ", ".join(used))); ok = False
            else:
                print("  ok   %s: %d generated FFI stencils were compiled into native blocks" % (name, len(used)))
    # a declaration that disagrees with the manifest must be refused, not run
    for what, decl, call in (("an int where the manifest has a long", "static extern int add_numbers(int a, long b);", "add_numbers(1, 2)"),
                             ("an int where the manifest has a string", "static extern int str_len(int s);", "str_len(1)"),
                             ("a string where the manifest has an int", "static extern int add_numbers(string a, int b);", 'add_numbers("x", 2)')):
        bad = os.path.join(out, "FfiMismatch.cs"); badexe = os.path.join(out, "FfiMismatch.exe")
        open(bad, "w").write("using System; using System.Runtime.InteropServices;\n"
                             "class P { [DllImport(\"mylib\")] %s\n"
                             "  static int Main() { Console.WriteLine(%s); return 0; } }\n" % (decl, call))
        c = subprocess.run(["mcs", "-nostdlib", "-r:" + os.path.join(out, "corlib.dll"), "-out:" + badexe, bad], capture_output=True, text=True)
        d = subprocess.run([binary, badexe], cwd=out, capture_output=True, text=True)
        if c.returncode == 0 and d.returncode != 0 and "disagree" in (d.stdout + d.stderr):
            print("  ok   a DllImport with %s is refused" % what)
        else:
            print("  FAIL: a mismatched DllImport (%s) was not refused (rc=%s): %s" % (what, d.returncode, (d.stdout + d.stderr)[:200])); ok = False
    return ok


def stencil_alias_groups():
    """The JIT recognises an instruction by the address of its handler, and instructions that share a handler body (JIT_LOAD_I64
    and JIT_LOAD_F64, say) do NOT share an address. So where the classification in JIT.c compares one member of such a group, it must
    compare all of them, or the missed ones are never made native: still the right answer, only slower, which nothing that compares
    output can see (ldelem.r4 and ldc.r8 were both missed this way)."""
    import re
    lines = open(os.path.join(SRC, "JIT_Execute.c")).read().split("\n")
    groups, cur = [], []
    for l in lines:
        m = re.match(r"^(JIT_\w+)_start:", l)
        if m:
            cur.append(m.group(1))
        elif l.strip() == "" or l.strip().startswith("//"):
            continue
        else:
            if len(cur) > 1:
                groups.append(cur)
            cur = []
    jit = open(os.path.join(SRC, "JIT.c")).read()
    used = set(re.findall(r"Translate\((JIT_\w+),", jit))
    for tbl in ("simpleOps", "lbccOps", "dbccOps", "fbccOps", "islandOps"):
        m = re.search(tbl + r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", jit, re.S)
        if m:
            used |= set(re.findall(r"JIT_\w+", m.group(1)))
    gen = os.path.join(SRC, "JIT_Fused.gen.h")
    if os.path.exists(gen):
        used |= set(re.findall(r"\{ (JIT_\w+),", open(gen).read()))
    used |= set(re.findall(r"JIT_(?:LOADPARAMLOCAL|STOREPARAMLOCAL)_[0-7]", jit))
    bad = [(sorted(o for o in g if o in used), sorted(o for o in g if o not in used)) for g in groups
           if any(o in used for o in g) and not all(o in used for o in g)]
    if bad:
        for have, missing in bad:
            print("  FAIL: JIT.c compares %s but not %s, which share its handler body" % (", ".join(have), ", ".join(missing)))
        return False
    print("  ok   %d groups of instructions that share a handler: wherever one is compared, all are" % len(groups))
    return True


TESTS = [("heaptree_difftest (C reference vs cpprust-lowered C++)", heaptree_difftest),
         ("metadata layout (generated) is current", metadata_layout_current),
         ("native parameter reads match their signatures", internalcall_params_ok),
         ("JIT.c: no conditionally-initialised locals", jit_uninitialised_locals),
         ("dotnet programs on " + os.path.basename(DNA_BIN), dotnet_programs),
         ("the register stencil test is generated from the current table", register_tests_in_sync),
         ("every stencil is exercised by a stencil test", stencil_coverage),
         ("native FFI: DllImport of C functions named in a manifest", ffi_calls),
         ("calls are inlined into native blocks", inlining_happens),
         ("instructions run as islands inside native blocks", islands_happen),
         ("stencil classification compares every alias of a handler", stencil_alias_groups),
         ("IL programs (opcodes C# does not emit), DNA vs Mono", il_programs)]

if __name__ == "__main__":
    failed = 0
    for name, fn in TESTS:
        print("==", name)
        if not fn():
            print("FAIL"); failed += 1
    sys.exit(1 if failed else 0)
