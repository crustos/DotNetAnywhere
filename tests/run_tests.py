#!/usr/bin/env python3
"""Run DotNetAnywhere tests. Requires a prior `python build.py`."""
import glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
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


def dotnet_programs():
    """Compile tests/dotnet/*.cs against corlib.dll and run them on build/dna32."""
    import shutil
    dna = os.path.join(BUILD, "dna32")
    if not shutil.which("mcs") or not os.path.exists(dna) \
            or not os.path.exists(os.path.join(BUILD, "corlib.dll")):
        print("  SKIP (needs: mcs, `python build.py --m32 --corlib`)")
        return True
    expected = {"GcStress": "live nodes: 5000\nchecksum: 91323100"}
    # Programs whose expectations must also hold on the reference runtime. Run under Mono
    # too, so a wrong expectation is caught there and not baked into DNA's behaviour.
    # (MarshalRefusal is deliberately DNA-only: the reference Marshal accepts strings.)
    parity = {"ManyLocalsAndArgs", "MarshalLayout", "ExceptionUnwind", "DictionaryConstrained", "ListBounds", "NullFields",
              "Int64AndFloatOps", "RefAccess", "ConversionChains",
              "DelegateVirtual", "TypedReferences"}
    # Generated programs that print one line per case: DNA's whole stdout must equal Mono's.
    output_parity = {"CheckedConversions", "UncheckedConversions", "ArithmeticMatrix", "UnboxChecks", "EnumFormatting", "ValueTypeBases", "ExceptionFilters"}
    out = os.path.join(BUILD, "dotnet"); os.makedirs(out, exist_ok=True)
    shutil.copy(os.path.join(BUILD, "corlib.dll"), out)
    ok = True
    for cs in sorted(glob.glob(os.path.join(ROOT, "tests", "dotnet", "*.cs"))):
        name = os.path.basename(cs)[:-3]
        dna_only_cs = name.endswith(".dnaonly")   # self-checking; the reference runtime cannot run it
        exe = os.path.join(out, name + ".exe")
        r = subprocess.run(["mcs", "-nostdlib", "-r:" + os.path.join(out, "corlib.dll"),
                            "-nowarn:0219", "-out:" + exe, cs], capture_output=True, text=True)
        if r.returncode:
            print("  compile failed:", name, r.stdout + r.stderr); ok = False; continue
        ref_out = None
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
    dna = os.path.join(BUILD, "dna32")
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


TESTS = [("heaptree_difftest (C reference vs cpprust-lowered C++)", heaptree_difftest),
         ("JIT.c: no conditionally-initialised locals", jit_uninitialised_locals),
         ("dotnet programs on dna32", dotnet_programs),
         ("IL programs (opcodes C# does not emit), DNA vs Mono", il_programs)]

if __name__ == "__main__":
    failed = 0
    for name, fn in TESTS:
        print("==", name)
        if not fn():
            print("FAIL"); failed += 1
    sys.exit(1 if failed else 0)
