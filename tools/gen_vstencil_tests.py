#!/usr/bin/env python3
"""Generate tests/dotnet/StencilRegisters.cs: a program with a method for each shape of expression that the register stencils
(tools/gen_vstencils.py) are made for. The set is taken from the table the stencils are generated from, so each stencil that exists has a
method shaped for it: operands that are locals (L), literals (C), 64-bit literals (K, the constant pool), the result of another operation
(R: the register), and a value that is in memory because something that is not a register stencil made it (M: a field of an object);
results that go into a register (an operand of the next operation), into another local, or back into the first operand (a += b).
The program is compared with Mono's output and the stencil statistics say which stencils it compiled.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_vstencils as G           # noqa: E402  (importing it builds the table)

OUT = os.path.join(HERE, "..", "tests", "dotnet", "StencilRegisters.cs")

TYPES = {"vi": "int", "vl": "long", "vf": "float", "vd": "double"}
FIELD = {"vi": "fi", "vl": "fl", "vf": "ff", "vd": "fd"}
OPSYM = {"add": "+", "sub": "-", "mul": "*", "and": "&", "or": "|", "xor": "^", "div": "/", "shl": "<<", "shr": ">>"}
CMP = ["<", "<=", ">", ">=", "==", "!="]


def operand(fam, kind, role, shift_count=False):
    """C# for an operand of kind L, C, K, R or M; role 'a' (first) or 'b' (second)"""
    t = "int" if shift_count else TYPES[fam]
    f = "fi" if shift_count else FIELD[fam]
    if kind == "L":
        return role
    if kind == "C":
        lit = {"int": "5" if role == "a" else "3", "long": "7L" if role == "a" else "-9L", "float": "1.5f" if role == "a" else "-2.25f"}[t]
        return lit
    if kind == "K":
        return {"long": "0x123456789abcdL" if role == "a" else "-0x2468ace13579L", "double": "1.2345678901" if role == "a" else "-7.5e300"}[t]
    if kind == "R":
        return "(c + d)" if not shift_count else "(c & 15)"
    if kind == "M":
        return "o." + f
    raise ValueError(kind)


def bits(fam, expr):
    return {"vi": "(long)(%s)", "vl": "(%s)", "vf": "BitConverter.DoubleToInt64Bits((double)(%s))", "vd": "BitConverter.DoubleToInt64Bits(%s)"}[fam] % expr


def binary_method(n, fam, op, A, B, dsts):
    t = TYPES[fam]
    shift = op in ("shl", "shr", "shrun")
    ea, eb = operand(fam, A, "a"), operand(fam, B, "b", shift)
    if op == "shrun":
        expr = "(int)((uint)(%s) >> (%s))" % (ea, eb)
    else:
        expr = "(%s) %s (%s)" % (ea, OPSYM[op], eb)
    body = []
    if "r" in dsts:
        body.append("h = h * 31 + %s;" % bits(fam, expr))
    if "s" in dsts:
        body.append("%s x = %s; h ^= %s;" % (t, expr, bits(fam, "x")))
    if "m" in dsts:
        if op == "shrun":
            body.append("a = (int)((uint)a >> (%s)); h += a;" % eb)
        else:
            body.append("a %s= %s; h += %s;" % (OPSYM[op], eb, bits(fam, "a")))
    name = "%s_%s_%s%s" % (fam, op, A, B)
    return name, t, body


def method(name, t, body, extra=""):
    lines = ["    static long F_%s(%s a, %s b, %s c, %s d, Holder o) {" % (name, t, t, t, t),
             "        long h = 0;", "        for (int k = 0; k < 2; k++) {"]
    lines += ["            " + b for b in body]
    lines += ["        }", "        return h;", "    }"]
    return "\n".join(lines)


def main():
    tab = G.table
    methods = []        # (name, type, body)
    seen = set()
    # binary operators: one method for all the destinations of an (op, A, B)
    groups = {}
    for key in tab:
        fam = key[0]
        if fam in TYPES and len(key) == 5:
            _, op, A, B, D = key
            groups.setdefault((fam, op, A, B), set()).add(D)
    for (fam, op, A, B), dsts in sorted(groups.items()):
        name, t, body = binary_method(len(methods), fam, op, A, B, dsts)
        methods.append((name, t, body))
    # compare and branch: all six comparisons (and, for floating point, their negations, which are the unordered branches)
    cc = {}
    for key in tab:
        if key[0] in ("vicc", "vlcc", "vfcc", "vdcc"):
            cc.setdefault((key[0][:2], key[2], key[3]), True)
    for (fam, A, B), _ in sorted(cc.items()):
        ea, eb = operand(fam, A, "a"), operand(fam, B, "b")
        body = []
        for i, c in enumerate(CMP):
            body.append("if ((%s) %s (%s)) h += %d;" % (ea, c, eb, 1 << i))
            if fam in ("vf", "vd"):
                body.append("if (!((%s) %s (%s))) h += %d;" % (ea, c, eb, 64 << i))
        methods.append(("%s_cc_%s%s" % (fam, A, B), TYPES[fam], body))
    # brtrue / brfalse
    for key in tab:
        if key[0] == "vjt":
            _, name, A = key
            if name in ("jt", "jf"):
                e = operand("vi", A, "a")
                cond = "!= 0" if name == "jt" else "== 0"
                methods.append(("%s_%s" % (name, A), "int", ["if ((%s) %s) h += 1; else h += 2;" % (e, cond)]))
            else:
                if A == "L":
                    # (the compiler branches on the inverse: `o != null` is a brfalse, `o == null` a brtrue)
                    methods.append(("%s_%s" % (name, A), "int", ["if (o != null) h += 1; else h += 2;", "if (o == null) h += 4; else h += 8;"]))
    # conversions
    for key in tab:
        if key[0] == "vcvt":
            _, name, A = key
            src = {"cvtil": "vi", "cvtul": "vi", "cvtif": "vi", "cvtid": "vi", "cvtfd": "vf", "cvtdf": "vd"}[name]
            e = operand(src, A, "a")
            conv = {"cvtil": ("(long)(%s)", "vl"), "cvtul": ("(long)(ulong)(uint)(%s)", "vl"), "cvtif": ("(float)(%s)", "vf"),
                    "cvtid": ("(double)(%s)", "vd"), "cvtfd": ("(double)(%s)", "vd"), "cvtdf": ("(float)(%s)", "vf")}[name]
            body = ["h = h * 31 + %s;" % bits(conv[1], conv[0] % e), "%s x = %s; h ^= %s;" % (TYPES[conv[1]], conv[0] % e, bits(conv[1], "x"))]
            methods.append(("cvt_%s_%s" % (name, A), TYPES[src], body))
    # stores and pushes (a push is a flush: before a call that is not inlined)
    for key in tab:
        if key[0] == "vst":
            _, cls, A = key
            fam = {"i": "vi", "l": "vl", "f": "vf", "d": "vd"}[cls]
            t = TYPES[fam]
            if A == "L":
                body = ["%s x = a; h ^= %s;" % (t, bits(fam, "x"))]
            elif A == "C":
                body = ["%s x = %s; h ^= %s;" % (t, operand(fam, "C", "a"), bits(fam, "x"))]
            elif A == "K":
                body = ["%s x = %s; h ^= %s;" % (t, operand("vl" if cls == "l" else "vd", "K", "a"), bits(fam, "x"))]
            else:
                body = ["%s x = c; h ^= %s;" % (t, bits(fam, "x"))]
            methods.append(("st_%s_%s" % (cls, A), t, body))
        if key[0] == "vpush":
            _, cls, A = key
            fam = {"i": "vi", "l": "vl", "f": "vf", "d": "vd"}[cls]
            t = TYPES[fam]
            e = {"L": "a", "C": operand(fam, "C", "a") if fam != "vd" else "1", "K": operand("vl", "K", "a"), "R": "(c + d)"}[A]
            methods.append(("push_%s_%s" % (cls, A), t, ["h = h * 31 + Sink_%s(%s);" % (fam, e)]))
    # shapes of whole expressions: what the register pass has to get right between the operations. Two intermediate values alive at once (only
    # one register: one is spilled), a store to a local that a pending operand still describes (a + (a = b): the operand must be read first), a
    # value on the stack where two paths meet (c ? x : y), and values held across a call
    for fam in ("vi", "vl", "vf", "vd"):
        t = TYPES[fam]
        ops = ["+", "-", "*"] + (["&", "|", "^"] if fam in ("vi", "vl") else [])
        body = []
        for i, op in enumerate(ops):
            o2 = ops[(i + 1) % len(ops)]
            o3 = ops[(i + 2) % len(ops)]
            body += ["h = h * 31 + %s;" % bits(fam, "((a %s b) %s (c %s d))" % (op, o2, o3)),
                     "h = h * 31 + %s;" % bits(fam, "(((a %s b) %s (c %s d)) %s (a %s d))" % (op, o2, o3, op, o3)),
                     "h = h * 31 + %s;" % bits(fam, "(a %s (a = b)) %s a" % (op, o2)),
                     "h = h * 31 + %s;" % bits(fam, "((c = a) %s c) %s (b %s c)" % (op, o2, o3)),
                     "h = h * 31 + %s;" % bits(fam, "((c = a %s b) %s c)" % (op, o2)),
                     "h = h * 31 + %s;" % bits(fam, "((d = a %s b) %s (d = c %s d))" % (op, o2, o3)),
                     "a = a %s (a = d) %s 2; h += %s;" % (op, o2, bits(fam, "a")),
                     "h = h * 31 + %s;" % bits(fam, "(a < b ? a %s 1 : b %s 2)" % (op, o2)),
                     "h = h * 31 + %s;" % bits(fam, "(a > b ? (c %s d) : (c %s d)) %s a" % (op, o2, o3)),
                     "h = h * 31 + %s;" % bits(fam, "a %s Sink_%s(b) %s c %s d" % (op, fam, o2, o3)),
                     "h = h * 31 + ((a %s b) != c ? 1 : 2);" % op if fam != "vf" else "h += 1;"]
            body.append("{ %s u = a, w = b; u = u %s (w = u %s w); h ^= %s; h ^= %s; }" % (t, op, o2, bits(fam, "u"), bits(fam, "w")))
        methods.append(("shape_%s" % fam, t, body))
    # the file
    out = ["// GENERATED by tools/gen_vstencil_tests.py. Do not edit: change the generator and regenerate.",
           "// One method for each shape of expression that a register stencil is for (see tools/gen_vstencils.py): %d methods." % len(methods),
           "using System;", "using System.Runtime.CompilerServices;", "",
           "public class Holder { public int fi; public long fl; public float ff; public double fd; public object fo; }", "",
           "public class Program {",
           "    [MethodImpl(MethodImplOptions.NoInlining)] static long Sink_vi(int x) { return x; }",
           "    [MethodImpl(MethodImplOptions.NoInlining)] static long Sink_vl(long x) { return x; }",
           "    [MethodImpl(MethodImplOptions.NoInlining)] static long Sink_vf(float x) { return BitConverter.DoubleToInt64Bits((double)x); }",
           "    [MethodImpl(MethodImplOptions.NoInlining)] static long Sink_vd(double x) { return BitConverter.DoubleToInt64Bits(x); }", ""]
    for name, t, body in methods:
        out.append(method(name, t, body))
    out += ["",
            "    static readonly int[] IV = { 0, 1, -1, 2, 7, 31, 32, 33, 100, -100, 65535, int.MinValue, int.MaxValue, 123456789, -987654321 };",
            "    static readonly long[] LV = { 0, 1, -1, 2, 7, 63, 64, 65, 0x7fffffffL, 0x80000000L, -0x80000001L, long.MinValue, long.MaxValue, 0x123456789abcdL, -0x2468ace13579L };",
            "    static readonly float[] FV = { 0f, -0f, 1f, -1f, 1.5f, -2.25f, 3.4e38f, float.NaN, float.PositiveInfinity, float.NegativeInfinity, 1e-30f, 16777217f };",
            "    static readonly double[] DV = { 0, -0.0, 1, -1, 1.5, -2.25, 1.2345678901, -7.5e300, double.NaN, double.PositiveInfinity, double.NegativeInfinity, 1e-300, 9007199254740993.0 };",
            "",
            "    public static void Main() {",
            "        Holder o = new Holder();"]
    for name, t, body in methods:
        fam = {"int": "IV", "long": "LV", "float": "FV", "double": "DV"}[t]
        out.append("        { long h = 0; for (int i = 0; i < %s.Length; i++) for (int j = 0; j < %s.Length; j++) {" % (fam, fam))
        out.append("            o.fi = IV[(i + j) % IV.Length]; o.fl = LV[(i * 3 + j) % LV.Length]; o.ff = FV[(i + 2 * j) % FV.Length]; o.fd = DV[(i + j * 5) % DV.Length]; o.fo = ((i + j) & 1) == 0 ? o : null;")
        out.append("            h = h * 1000003 + F_%s(%s[i], %s[j], %s[(i + j) %% %s.Length], %s[(i * 2 + 1) %% %s.Length], o); }" % (name, fam, fam, fam, fam, fam, fam))
        out.append('          Console.WriteLine("%s " + h); }' % name)
    out += ["    }", "}", ""]
    text = "\n".join(out)
    if "--check" in sys.argv:
        same = os.path.exists(OUT) and open(OUT).read() == text
        print("gen_vstencil_tests: %s" % ("tests/dotnet/StencilRegisters.cs is up to date" if same else "tests/dotnet/StencilRegisters.cs is OUT OF DATE: run tools/gen_vstencil_tests.py"))
        sys.exit(0 if same else 1)
    open(OUT, "w").write(text)
    print("gen_vstencil_tests: %d methods, %d lines" % (len(methods), text.count("\n")))


if __name__ == "__main__":
    main()
