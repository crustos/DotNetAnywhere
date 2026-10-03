#!/usr/bin/env python3
"""Crust C#-subset conformance for DotNetAnywhere.

Extracts every C# program (a string literal containing `int Main(`) from
Crust's tests, then for each one:

  1. reference: compile with mcs against Mono's mscorlib, run under `mono`.
     Mono is the oracle: its exit code is what the program should return (Crust's
     tests assert specific codes, not just 0). Programs Mono can't compile or run
     are skipped, with the reason.
  2. dna-compile: compile against DNA's corlib.dll. Failure = a corlib API gap.
  3. dna-run / dna-wrong: run on build/dna32. dna-run = DNA crashed; dna-wrong =
     it ran but returned a different exit code than Mono.

Usage:
  python tests/crust_conformance.py [--crust ../crust] [-j N] [-v] [--only SUBSTR]
Requires: mcs, mono, and `python build.py --m32 --corlib`.
"""
import argparse, ast, collections, concurrent.futures as cf, glob, hashlib, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
WORK = os.path.join(BUILD, "conformance")
# runtime under test: build/dna32 by default, `--64` or DNA_BIN=path for the 64-bit build
if "--64" in sys.argv:
    sys.argv.remove("--64")
    os.environ["DNA_BIN"] = os.path.join(BUILD, "dna")
DNA_BIN = os.environ.get("DNA_BIN", os.path.join(BUILD, "dna32"))


# Crust's blittable tests build `public int Run()` programs with a _program() helper
# (and the PACKET constant); the C harness calls Run, so give them a Main that does.
def run_entry(text):
    """A Main that calls Run() on whichever class declares it (and in its namespace)."""
    m = re.search(r"public\s+int\s+Run\s*\(", text)
    classes = list(re.finditer(r"\b(?:class|struct)\s+(\w+)", text[:m.start()]))
    cls = classes[-1].group(1) if classes else "Program"
    ns = [n for n in re.finditer(r"\bnamespace\s+([\w.]+)", text[:m.start()])]
    full = (ns[-1].group(1) + "." + cls) if ns else cls
    return "\npublic class __CrustEntry { public static int Main() { return new %s().Run(); } }\n" % full
MM_HEAD = "using System.Runtime.InteropServices;\n"   # _MM_HEAD in test_csrust.py


def ev(node):
    """Evaluate a string-valued expression the way Crust's tests build programs: literals,
    `+` concatenation, and the `_program(body, types)` helper. None if it is anything else."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        l, r = ev(node.left), ev(node.right)
        return l + r if l is not None and r is not None else None
    if isinstance(node, ast.Call) and getattr(node.func, "id", None) == "_program" and node.args:
        body = ev(node.args[0])
        if len(node.args) > 1:
            types = ev(node.args[1])
        else:
            kw = next((k for k in node.keywords if k.arg == "types"), None)
            types = ev(kw.value) if kw else ""
        if body is None or types is None:
            return None
        return (MM_HEAD + types + "public class Program {\n    public int Run() {\n"
                + body + "\n    }\n}\n")
    return None


def extract(crust):
    progs, seen = [], set()
    skipped_refusals = []

    def add(f, lineno, text):
        if re.search(r"public\s+int\s+Run\s*\(", text) and not re.search(r"\bint\s+Main\s*\(", text):
            text += run_entry(text)
        elif not re.search(r"\bint\s+Main\s*\(", text):
            return
        h = hashlib.sha1(text.encode()).hexdigest()[:8]
        if h not in seen:
            seen.add(h)
            progs.append(("%s:%d.%s" % (os.path.basename(f)[:-3], lineno, h), text))

    def visit(f, node):
        # Programs handed to assert_refuses(...) are ones Crust is meant to REFUSE (outside
        # its subset), so they say nothing about conformance. Skip them whole.
        if isinstance(node, ast.Call) and "refus" in getattr(node.func, "attr", getattr(node.func, "id", "")):
            skipped_refusals.append(node.lineno)
            return
        # maximal evaluable expression: take it whole, don't also take its pieces
        text = ev(node) if isinstance(node, (ast.Constant, ast.BinOp, ast.Call)) else None
        if text is not None:
            add(f, node.lineno, text)
            return
        for child in ast.iter_child_nodes(node):
            visit(f, child)

    for f in sorted(glob.glob(os.path.join(crust, "tests", "test_cs*.py"))):
        visit(f, ast.parse(open(f).read()))
    extract.refusals = len(skipped_refusals)
    return progs


def sh(cmd, cwd=None, timeout=20):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"


def clean(out):
    return "\n".join(l for l in out.splitlines() if l.strip() and not l.startswith("Total execution time"))


def classify_dna(rc, out):
    out = clean(out)
    m = re.search(r"\*\*\* CRASH \*\*\*\s*\n(.*)", out)
    if m:
        return "crash: " + re.sub(r"0x[0-9a-fA-F]+|\d+", "N", m.group(1).strip())[:90]
    if rc == 124:
        return "timeout"
    return "exit %d" % rc


# What Crust's harness supplies around a snippet: common usings and its [Shared] marker.
PRELUDE_HEAD = "using System;\nusing System.Collections.Generic;\n"
PRELUDE_TAIL = "\npublic class SharedAttribute : System.Attribute { }\n"
MCS_WARN = "-nowarn:0219,0414,0649,0168,0169,0162,0108,0067"


def hoist_usings(src):
    """Crust's tests concatenate `types + _program(..)`, which can leave a `using` after a
    declaration; C# needs them first. (Crust's own front end is happy either way.)"""
    lines = src.split("\n")
    usings = []
    rest = []
    for l in lines:
        if re.match(r"^using\s+[\w.]+\s*;\s*$", l):
            if l.strip() not in usings:
                usings.append(l.strip())
        else:
            rest.append(l)
    return "\n".join(usings + rest)


def run_one(item):
    name, src = item
    src = hoist_usings(src)
    d = os.path.join(WORK, name.replace(":", "_"))
    os.makedirs(d, exist_ok=True)
    cs = os.path.join(d, "p.cs")
    open(cs, "w").write(src)
    # 1. reference
    rc, out = sh(["mcs", MCS_WARN, "-unsafe", "-out:ref.exe", "p.cs"], cwd=d)
    if rc:
        # retry with the prelude Crust's harness adds around a snippet
        src = PRELUDE_HEAD + src + PRELUDE_TAIL
        cs2 = os.path.join(d, "p.cs")
        open(cs2, "w").write(src)
        rc, out2 = sh(["mcs", MCS_WARN, "-unsafe", "-out:ref.exe", "p.cs"], cwd=d)
        if rc:
            first = [l for l in out.splitlines() if "error CS" in l]
            return name, "skip", "mono-compile: " + re.sub(r"`[^']*'", "`X'", (first or ["?"])[0].split(": ", 1)[-1])[:80]
    ref_rc, out = sh(["mono", "ref.exe"], cwd=d)
    ref_exc = None
    if ref_rc == 124:
        return name, "skip", "mono timed out"
    if "Unhandled Exception" in out:
        # Crust's C aborts where .NET throws; parity means DNA dies with the same exception.
        m = re.search(r"Unhandled Exception:\s*([\w.]+)", out)
        ref_exc = m.group(1) if m else "?"
    elif ref_rc < 0:
        return name, "skip", "mono crashed"
    # 2. against DNA corlib
    shutil.copy(os.path.join(BUILD, "corlib.dll"), d)
    rc, out = sh(["mcs", "-nostdlib", "-r:corlib.dll", "-unsafe", "-nowarn:0219,0414,0649,0168,0169,0162,0108,0067",
                  "-out:dna.exe", "p.cs"], cwd=d)
    if rc:
        errs = re.findall(r"error (CS\d+): (.*)", out)
        key = errs[0] if errs else ("?", out.strip()[:80])
        return name, "dna-compile", "%s %s" % (key[0], re.sub(r"`[^']*'", "`X'", key[1])[:90])
    # 3. run on DNA
    rc, out = sh([DNA_BIN, "dna.exe"], cwd=d)
    if ref_exc is not None:
        m = re.search(r"Unhandled exception in [^:\n]*:\s*([\w.]+)", out)
        got = m.group(1) if m else None
        if got == ref_exc:
            return name, "pass", ""
        return name, "dna-wrong", "unhandled %s, Mono gives %s" % (got or ("exit %d" % rc), ref_exc)
    if rc == ref_rc:
        return name, "pass", ""
    # Crust refuses CreateSpan over more than one element at compile time (the span would run
    # past a single variable); the reference runtime just reads out of bounds. DNA refuses at
    # run time with NotSupportedException. Intentional, so not counted as a gap.
    if "CreateSpan(): System.NotSupportedException" in out and re.search(r"CreateSpan\(ref \w+,\s*(?!1\))", src):
        return name, "by-design", "CreateSpan length != 1 refused (Crust refuses it too)"
    if "*** CRASH ***" in out or rc < 0:
        return name, "dna-run", classify_dna(rc, out)
    return name, "dna-wrong", "exit %d, Mono gives %d" % (rc, ref_rc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--crust", default=os.path.join(ROOT, "..", "crust"))
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--only")
    a = ap.parse_args()
    for need in ("mcs", "mono"):
        if not shutil.which(need):
            sys.exit("need %s (apt install mono-mcs mono-runtime)" % need)
    for f in (os.path.basename(DNA_BIN), "corlib.dll"):
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit("missing build/%s; run: python build.py --m32 --corlib" % f)
    progs = extract(a.crust)
    if a.only:
        progs = [p for p in progs if a.only in p[0] or a.only in p[1]]
    shutil.rmtree(WORK, ignore_errors=True)
    with cf.ThreadPoolExecutor(a.j) as ex:
        results = list(ex.map(run_one, progs))
    tally = collections.Counter(r[1] for r in results)
    ran = [r for r in results if r[1] != "skip"]
    print("programs extracted: %d (+%d passed to Crust's assert_refuses, excluded: outside its subset)"
          % (len(results), getattr(extract, "refusals", 0)))
    print("usable (compile and run under Mono): %d" % len(ran))
    for k in ("pass", "by-design", "dna-compile", "dna-run", "dna-wrong"):
        print("  %-12s %d" % (k, tally[k]))
    for stage in ("dna-compile", "dna-run", "dna-wrong"):
        c = collections.Counter(r[2] for r in results if r[1] == stage)
        if c:
            print("\n%s failures by cause:" % stage)
            for cause, n in c.most_common():
                print("  %3d  %s" % (n, cause))
    if a.v:
        print()
        for r in results:
            print("%-12s %-34s %s" % (r[1], r[0], r[2]))
    skips = collections.Counter(re.sub(r"\d+", "N", r[2]) for r in results if r[1] == "skip")
    if skips:
        print("\nskipped (Mono could not compile/run them):")
        for cause, n in skips.most_common():
            print("  %3d  %s" % (n, cause))
    return 0 if not (tally["dna-compile"] or tally["dna-run"] or tally["dna-wrong"]) else 1


if __name__ == "__main__":
    sys.exit(main())
