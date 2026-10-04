#!/usr/bin/env python3
"""Generate the register stencils: native/stencils/vstencils.gen.c, native/stencils/vstencils.json and native/src/VStencils.gen.h

The plain stencils (stencils.c) each do what one instruction does to the evaluation stack in memory, so `x = a + b` is four of them and a
trip through memory for each value. These are the three-address stencils that the register pass in JIT.c (EmitBlock) puts in their place:
it keeps a *virtual* evaluation stack, in which a load of a local or a constant is only a description ("the int at frame offset 8", "the
constant 3"), and an operator takes its operands from where they are and leaves its result in a register (eax / rax / xmm0) or, if the next
instruction stores it, writes it where it goes. One stencil does that, and there is one for each operator and each combination of where
the operands are:

    L  a local (a frame offset; HOLE operand)     C  a constant (an immediate; HOLE operand)     R  the cached register
    M  the top of the evaluation stack in memory (popped by the stencil; only as the first operand: it is what was spilled)
    K  a 64-bit constant, from the block's constant pool (a RIP-relative memory operand; HOLE5, whose pool index is value hole 2); only
       for double and long

and where the result goes: r (the register), s (a local: the destination is a third operand), m (read-modify-write: `a += b` on a local).
Operands are numbered in the order of the stencil's value holes: the first operand that is not R takes HOLE0, the next HOLE3, the destination
(for s) the one after (HOLE4). Registers: eax / rax and xmm0 are the cache; ecx / rcx and xmm1 hold a second operand; r10 is the scratch for pushing
a value to the memory stack without touching the cache (r11 and eax are free in the float compare stencils).

The names are v<type>_<op>_<A><B>_<dst>: vi (int32), vl (int64), vf (float32), vd (double). A table in VStencils.gen.h gives the stencil for
each (operator, A, B, dst), for JIT.c.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "..", "native", "stencils", "vstencils.gen.c")
OUT_JSON = os.path.join(HERE, "..", "native", "stencils", "vstencils.json")
OUT_H = os.path.join(HERE, "..", "native", "src", "VStencils.gen.h")

KINDS = "LCRMK"
HOLES = ["HOLE0", "HOLE3", "HOLE4"]

funcs = []          # (name, [asm lines])
table = {}          # (family, key tuple) -> name


class H:
    def __init__(self):
        self.n = 0

    def next(self):
        s = HOLES[self.n]
        self.n += 1
        return s


def emit(name, lines):
    funcs.append((name, lines))
    return name


# ------------------------------------------------------------------------------------------------ integers
INT_OPS = [("add", "add", True), ("sub", "sub", False), ("mul", "imul", True), ("and", "and", True), ("or", "or", True), ("xor", "xor", True)]
SHIFT_OPS = [("shl", "shl"), ("shr", "sar"), ("shrun", "shr")]


def operand_text(kind, h, sz=4):
    """the text of a non-R operand: a local or a constant takes the next value hole, M is the top of the memory stack, K the pool"""
    if kind == "L":
        return "%s(%%r13)" % h.next()
    if kind == "C":
        return "$%s" % h.next()
    if kind == "M":
        return "-%d(%%r12)" % sz
    if kind == "K":
        return "HOLE5(%rip)"
    return None


def pop_a(A, sz):
    """after the use of a memory first operand: drop it from the memory stack (lea: it leaves the flags alone)"""
    return ["leaq -%d(%%r12), %%r12" % sz] if A == "M" else []


def valid(A, B, fam):
    """which operand kinds go together for a family (fam: vi vl vf vd)"""
    if B == "M" or (A == "R" and B == "R"):
        return False
    if A == "C" and B == "C":
        return False
    if "K" in (A, B):
        if fam not in ("vl", "vd") or (A == "K" and B in "CK") or (B == "K" and A in "CK"):
            return False
    if "C" in (A, B) and fam == "vd":
        return False
    return True


def int_binary(fam, op, mn, comm, A, B, D, wide):
    """result of A op B in eax/rax; D: r (stay), s (store to a local), m (A op= B, A a local)"""
    if not valid(A, B, "vl" if wide else "vi"):
        return None
    if D == "m" and not (A == "L" and op in ("add", "sub", "and", "or", "xor")):
        return None
    sfx, ax, cx = ("q", "%rax", "%rcx") if wide else ("l", "%eax", "%ecx")
    h = H()
    sz = 8 if wide else 4
    a = operand_text(A, h, sz)
    b = operand_text(B, h, sz)
    lines = []
    if D == "m":
        # a (a local) op= b, in memory
        if B == "C":
            lines.append("%s%s %s, %s" % (mn, sfx, b, a))
        elif B in "LK":
            lines += ["mov%s %s, %s" % (sfx, b, ax), "%s%s %s, %s" % (mn, sfx, ax, a)]
        else:
            lines.append("%s%s %s, %s" % (mn, sfx, ax, a))
        d = None
    else:
        if A == "R":
            src = b
            if op == "mul" and B == "C":
                lines.append("imul%s %s, %s, %s" % (sfx, b, ax, ax))
            else:
                lines.append("%s%s %s, %s" % (mn, sfx, b, ax))
        elif B == "R":
            if comm:
                if op == "mul" and A == "C":
                    lines.append("imul%s %s, %s, %s" % (sfx, a, ax, ax))
                else:
                    lines.append("%s%s %s, %s" % (mn, sfx, a, ax))
            else:           # sub: a - b, with b in the register
                lines += ["neg%s %s" % (sfx, ax), "add%s %s, %s" % (sfx, a, ax)]
        else:
            lines.append("mov%s %s, %s" % (sfx, a, ax))
            if op == "mul" and B == "C":
                lines.append("imul%s %s, %s, %s" % (sfx, b, ax, ax))
            else:
                lines.append("%s%s %s, %s" % (mn, sfx, b, ax))
        d = None
        if D == "s":
            d = "%s(%%r13)" % h.next()
            lines.append("mov%s %s, %s" % (sfx, ax, d))
    return lines + pop_a(A, sz)


def int_shift(op, mn, A, B, D):
    """int32 shift: the count is the second operand (an int32 for a long shift too, but those are not done here)"""
    if not valid(A, B, "vi") or D == "m":
        return None
    if B == "L":
        return None         # (a compiler masks a variable shift count with & 31, so the count is never a bare local: nothing to test it with)
    h = H()
    a = operand_text(A, h)
    b = operand_text(B, h)
    lines = []
    if A == "R":
        lines.append("movl %s, %%ecx" % b)
    elif B == "R":
        lines += ["movl %eax, %ecx", "movl %s, %%eax" % a]
    else:
        lines += ["movl %s, %%eax" % a, "movl %s, %%ecx" % b]
    lines.append("%sl %%cl, %%eax" % mn)
    if D == "s":
        lines.append("movl %%eax, %s(%%r13)" % h.next())
    return lines + pop_a(A, 4)


for D in "rsm":
    pass

for op, mn, comm in INT_OPS:
    for A in KINDS:
        for B in KINDS:
            for D in "rsm":
                lines = int_binary("vi", op, mn, comm, A, B, D, False)
                if lines is not None:
                    table[("vi", op, A, B, D)] = emit("vi_%s_%s%s_%s" % (op, A, B, D), lines)
                lines = int_binary("vl", op, mn, comm, A, B, D, True)
                if lines is not None:
                    table[("vl", op, A, B, D)] = emit("vl_%s_%s%s_%s" % (op, A, B, D), lines)
for op, mn in SHIFT_OPS:
    for A in KINDS:
        for B in KINDS:
            for D in "rs":
                lines = int_shift(op, mn, A, B, D)
                if lines is not None:
                    table[("vi", op, A, B, D)] = emit("vi_%s_%s%s_%s" % (op, A, B, D), lines)

# ------------------------------------------------------------------------------------------------ floating point
FP_OPS = [("add", "add", True), ("sub", "sub", False), ("mul", "mul", True), ("div", "div", False)]


def load_fp(x, kind, h, dbl):
    """load a non-R operand into xmm<x>"""
    sd = "sd" if dbl else "ss"
    if kind == "L":
        return ["mov%s %s(%%r13), %%xmm%d" % (sd, h.next(), x)]
    return ["movl $%s, %%ecx" % h.next(), "movd %%ecx, %%xmm%d" % x]            # a float32 constant: its bits


def fp_binary(op, mn, comm, A, B, D, dbl):
    if not valid(A, B, "vd" if dbl else "vf") or D == "m":
        return None
    sd = "sd" if dbl else "ss"
    sz = 8 if dbl else 4
    h = H()
    lines = []
    # the operands' holes are taken in order A then B whatever the instructions' order (M and K have none: the stack, the pool)
    a_hole = h.next() if A in "LC" else None
    b_hole = h.next() if B in "LC" else None

    def mem_text(kind, hole):
        return {"L": "%s(%%r13)" % hole, "M": "-%d(%%r12)" % sz, "K": "HOLE5(%rip)"}.get(kind)

    def ld(x, kind, hole):
        if kind in "LMK":
            return ["mov%s %s, %%xmm%d" % (sd, mem_text(kind, hole), x)]
        return ["movl $%s, %%ecx" % hole, "movd %%ecx, %%xmm%d" % x]

    def mem_or_reg1(kind, hole):
        """the operand as the source of an arithmetic instruction: memory, or (a constant) xmm1 loaded first"""
        if kind in "LMK":
            return [], mem_text(kind, hole)
        return ld(1, kind, hole), "%xmm1"

    if A == "R":
        pre, src = mem_or_reg1(B, b_hole)
        lines += pre + ["%s%s %s, %%xmm0" % (mn, sd, src)]
    elif B == "R":
        if comm:
            pre, src = mem_or_reg1(A, a_hole)
            lines += pre + ["%s%s %s, %%xmm0" % (mn, sd, src)]
        else:
            lines += ["movaps %xmm0, %xmm1"] + ld(0, A, a_hole) + ["%s%s %%xmm1, %%xmm0" % (mn, sd)]
    else:
        lines += ld(0, A, a_hole)
        pre, src = mem_or_reg1(B, b_hole)
        lines += pre + ["%s%s %s, %%xmm0" % (mn, sd, src)]
    if D == "s":
        lines.append("mov%s %%xmm0, %s(%%r13)" % (sd, h.next()))
    return lines + pop_a(A, sz)


for op, mn, comm in FP_OPS:
    for A in KINDS:
        for B in KINDS:
            for D in "rs":
                lines = fp_binary(op, mn, comm, A, B, D, False)
                if lines is not None:
                    table[("vf", op, A, B, D)] = emit("vf_%s_%s%s_%s" % (op, A, B, D), lines)
                lines = fp_binary(op, mn, comm, A, B, D, True)
                if lines is not None:
                    table[("vd", op, A, B, D)] = emit("vd_%s_%s%s_%s" % (op, A, B, D), lines)

# ------------------------------------------------------------------------------------------------ compare and branch
ICC = [("eq", "je"), ("ne", "jne"), ("lt", "jl"), ("le", "jle"), ("gt", "jg"), ("ge", "jge")]


def int_cc(cc, jcc, A, B, wide):
    if not valid(A, B, "vl" if wide else "vi"):
        return None
    if cc in ("eq", "ne") and A in "CK":
        return None         # (compilers put the constant second in a symmetric comparison; the plain stencil does the rest)
    sfx, ax, cx = ("q", "%rax", "%rcx") if wide else ("l", "%eax", "%ecx")
    h = H()
    sz = 8 if wide else 4
    a = operand_text(A, h, sz)
    b = operand_text(B, h, sz)
    lines = []
    if A == "R":
        lines.append("cmp%s %s, %s" % (sfx, b if B != "R" else ax, ax))
    elif B == "R":
        lines += ["mov%s %s, %s" % (sfx, a, cx), "cmp%s %s, %s" % (sfx, ax, cx)]           # a - b, b in the register
    elif A == "L" and B == "C":
        lines.append("cmp%s %s, %s" % (sfx, b, a))                                         # compare a local with a constant in memory
    else:
        lines += ["mov%s %s, %s" % (sfx, a, ax), "cmp%s %s, %s" % (sfx, b, ax)]
    lines += pop_a(A, sz)
    lines.append("%s HOLE1" % jcc)
    return lines


for cc, jcc in ICC:
    for A in KINDS:
        for B in KINDS:
            for fam, wide in (("vi", False), ("vl", True)):
                lines = int_cc(cc, jcc, A, B, wide)
                if lines is not None:
                    table[(fam + "cc", cc, A, B)] = emit("%s_j%s_%s%s" % (fam, cc, A, B), lines)

# float compare and branch: cond -> (which operand is compared against the other: "a" means a (the second from the top) is in the register, jcc)
FCC = {"lt": ("b", "ja"), "le": ("b", "jae"), "gt": ("a", "ja"), "ge": ("a", "jae"),
       "lt_un": ("a", "jb"), "le_un": ("a", "jbe"), "gt_un": ("b", "jb"), "ge_un": ("b", "jbe")}


def fp_cc(cc, A, B, dbl):
    if not valid(A, B, "vd" if dbl else "vf") or "C" in (A, B):
        return None
    if cc in ("eq", "ne") and A == "K":
        return None
    uc = "ucomisd" if dbl else "ucomiss"
    mv = "movsd" if dbl else "movss"
    sz = 8 if dbl else 4
    h = H()
    a = {"L": "%s(%%r13)" % h.next() if A == "L" else None, "M": "-%d(%%r12)" % sz, "K": "HOLE5(%rip)"}.get(A)
    b = {"L": "%s(%%r13)" % h.next() if B == "L" else None, "M": None, "K": "HOLE5(%rip)"}.get(B)
    lines = []
    if cc in ("eq", "ne"):
        # compare a with b, whichever way round; then the flags: equal is ZF=1 and PF=0, not equal is ZF=0 or PF=1
        if A == "R":
            lines.append("%s %s, %%xmm0" % (uc, b))
        elif B == "R":
            lines += ["%s %s, %%xmm1" % (mv, a), "%s %%xmm0, %%xmm1" % uc]
        else:
            lines += ["%s %s, %%xmm1" % (mv, a), "%s %s, %%xmm1" % (uc, b)]
        lines += pop_a(A, sz)
        if cc == "eq":
            lines += ["sete %r10b", "setnp %r11b", "test %r11b, %r10b", "jnz HOLE1"]
        else:
            lines += ["jne HOLE1", "jp HOLE1"]
        return lines
    which, jcc = FCC[cc]
    if which == "a":                     # a is the one compared against b (a in a register)
        if A == "R":
            lines.append("%s %s, %%xmm0" % (uc, b))
        elif B == "R":
            lines += ["%s %s, %%xmm1" % (mv, a), "%s %%xmm0, %%xmm1" % uc]
        else:
            lines += ["%s %s, %%xmm1" % (mv, a), "%s %s, %%xmm1" % (uc, b)]
    else:                                # b is compared against a
        if B == "R":
            lines.append("%s %s, %%xmm0" % (uc, a))
        elif A == "R":
            lines += ["%s %s, %%xmm1" % (mv, b), "%s %%xmm0, %%xmm1" % uc]
        else:
            lines += ["%s %s, %%xmm1" % (mv, b), "%s %s, %%xmm1" % (uc, a)]
    lines += pop_a(A, sz)
    lines.append("%s HOLE1" % jcc)
    return lines


for cc in ["eq", "ne"] + list(FCC):
    for A in KINDS:
        for B in KINDS:
            for fam, dbl in (("vf", False), ("vd", True)):
                lines = fp_cc(cc, A, B, dbl)
                if lines is not None:
                    table[(fam + "cc", cc, A, B)] = emit("%s_j%s_%s%s" % (fam, cc, A, B), lines)

# brtrue / brfalse
for name, cond, wide in (("jt", "jne", False), ("jf", "je", False), ("jt8", "jne", True), ("jf8", "je", True)):
    for A in ("LR" if not wide else "L"):       # (an 8-byte value that is tested is a reference that was loaded, never a computed one)
        h = H()
        sfx, ax = ("q", "%rax") if wide else ("l", "%eax")
        if A == "L":
            lines = ["cmp%s $0, %s(%%r13)" % (sfx, h.next())]
        else:
            lines = ["test%s %s, %s" % (sfx, ax, ax)]
        lines.append("%s HOLE1" % cond)
        table[("vjt", name, A)] = emit("v_%s_%s" % (name, A), lines)

# ------------------------------------------------------------------------------------------------ conversions
# (a constant is converted when the stencil is chosen, so there is no C form, except int to floating point)
CVT = [
    ("cvtil", "i", "l"), ("cvtul", "i", "l"), ("cvtif", "i", "f"), ("cvtid", "i", "d"), ("cvtfd", "f", "d"), ("cvtdf", "d", "f"),
]
for name, src, dst in CVT:
    for A in "LCR":
        if A == "C":
            continue            # (a constant is converted when the stencil is chosen (cvtil cvtul cvtif), or by the plain stencil (the rest))
        h = H()
        if name == "cvtil":
            lines = ["movslq %s(%%r13), %%rax" % h.next()] if A == "L" else ["cltq"]
        elif name == "cvtul":
            lines = ["movl %s(%%r13), %%eax" % h.next()] if A == "L" else ["movl %eax, %eax"]
        elif name == "cvtif":
            lines = {"L": ["xorps %xmm0, %xmm0", "cvtsi2ssl %s(%%r13), %%xmm0" % h.next()], "R": ["xorps %xmm0, %xmm0", "cvtsi2ss %eax, %xmm0"],
                     "C": ["movl $%s, %%eax" % h.next(), "xorps %xmm0, %xmm0", "cvtsi2ss %eax, %xmm0"]}[A]
        elif name == "cvtid":
            lines = {"L": ["xorps %xmm0, %xmm0", "cvtsi2sdl %s(%%r13), %%xmm0" % h.next()], "R": ["xorps %xmm0, %xmm0", "cvtsi2sd %eax, %xmm0"],
                     "C": ["movl $%s, %%eax" % h.next(), "xorps %xmm0, %xmm0", "cvtsi2sd %eax, %xmm0"]}[A]
        elif name == "cvtfd":
            lines = ["xorps %xmm0, %xmm0", "cvtss2sd %s(%%r13), %%xmm0" % h.next()] if A == "L" else ["cvtss2sd %xmm0, %xmm0"]
        else:
            lines = ["xorps %xmm0, %xmm0", "cvtsd2ss %s(%%r13), %%xmm0" % h.next()] if A == "L" else ["cvtsd2ss %xmm0, %xmm0"]
        table[("vcvt", name, A)] = emit("v_%s_%s" % (name, A), lines)

# ------------------------------------------------------------------------------------------------ stores and pushes
for A, kinds in (("i", "LCR"), ("l", "LCR"), ("f", "LCR"), ("d", "LR")):
    pass
STORES = {
    ("i", "L"): ["movl {a}, %r10d", "movl %r10d, {d}"], ("i", "C"): ["movl ${a}, {d}"], ("i", "R"): ["movl %eax, {d}"],
    ("l", "L"): ["movq {a}, %r10", "movq %r10, {d}"], ("l", "C"): ["movq ${a}, {d}"], ("l", "R"): ["movq %rax, {d}"],
    ("l", "K"): ["movq HOLE5(%rip), %r10", "movq %r10, {d}"],
    ("f", "R"): ["movss %xmm0, {d}"], ("d", "R"): ["movsd %xmm0, {d}"],
}
for (cls, A), tpl in STORES.items():
    h = H()
    a = h.next() if A in "LC" else None
    d = "%s(%%r13)" % h.next()
    a_text = ("%s(%%r13)" % a) if A == "L" else a
    lines = [t.format(a=a_text if A == "L" else a, d=d) for t in tpl]
    table[("vst", cls, A)] = emit("v_st_%s_%s" % (cls, A), lines)

PUSHES = {
    ("i", "L"): ["movl {a}, %r10d", "movl %r10d, (%r12)", "addq $4, %r12"], ("i", "C"): ["movl ${a}, (%r12)", "addq $4, %r12"], ("i", "R"): ["movl %eax, (%r12)", "addq $4, %r12"],
    ("l", "L"): ["movq {a}, %r10", "movq %r10, (%r12)", "addq $8, %r12"], ("l", "C"): ["movq ${a}, (%r12)", "addq $8, %r12"], ("l", "R"): ["movq %rax, (%r12)", "addq $8, %r12"],
    ("l", "K"): ["movq HOLE5(%rip), %r10", "movq %r10, (%r12)", "addq $8, %r12"],
    ("f", "R"): ["movss %xmm0, (%r12)", "addq $4, %r12"], ("d", "R"): ["movsd %xmm0, (%r12)", "addq $8, %r12"],
}
for (cls, A), tpl in PUSHES.items():
    h = H()
    a = h.next() if A in "LC" else None
    a_text = ("%s(%%r13)" % a) if A == "L" else a
    lines = [t.format(a=a_text) for t in tpl]
    table[("vpush", cls, A)] = emit("v_push_%s_%s" % (cls, A), lines)


# ------------------------------------------------------------------------------------------------ output
def c_source():
    out = ["// GENERATED by tools/gen_vstencils.py. Do not edit: change the templates there and regenerate.", ""]
    for name, lines in funcs:
        body = "\n".join('\t"%s\\n"' % l.replace("%", "%") for l in lines + ["ret"])
        out.append("__attribute__((naked)) void st_%s(void) {\n\t__asm__(\n%s);\n}\n" % (name, body))
    return "\n".join(out)


def check_tables(text):
    """every table must have exactly as many initialisers as its dimensions (C pads a short one with zeros, which is the id of ldl)"""
    import re
    for m in re.finditer(r"static const short (\w+)((?:\[\d+\])+) = \{(.*?)\n?\s*\};", text, re.S):
        dims = [int(x) for x in re.findall(r"\[(\d+)\]", m.group(2))]
        want = 1
        for d in dims:
            want *= d
        got = len(re.findall(r"ST_\w+|-1", m.group(3)))
        if got != want:
            sys.exit("gen_vstencils: table %s has %d entries, expected %d" % (m.group(1), got, want))


def header():
    # the stencil of each family, indexed [op][A][B][dst] etc., as ST_ ids (the enum of Stencils.gen.h comes first)
    kidx = {"L": 0, "C": 1, "R": 2}
    didx = {"r": 0, "s": 1, "m": 2}
    o = ["// GENERATED by tools/gen_vstencils.py. Do not edit.", "// The register stencils: kinds of operand L (a local) C (a constant) R (the register); dst r (register) s (a local) m (a += b on a local)",
         "// -1 where there is no stencil for a combination.", "", "enum { VK_L = 0, VK_C = 1, VK_R = 2, VK_M = 3, VK_K = 4 };", ""]

    def st(n):
        return "ST_" + n.upper() if n else "-1"

    def arr3(fam, ops, dsts):
        # [op][A][B][dst]
        rows = []
        for op in ops:
            ra = []
            for A in KINDS:
                rb = []
                for B in KINDS:
                    rb.append("{ %s }" % ", ".join(st(table.get((fam, op, A, B, d))) for d in dsts))
                ra.append("{ %s }" % ", ".join(rb))
            rows.append("{ %s }" % ",\n\t\t".join(ra))
        return ",\n\t".join(rows)

    int_ops = [x[0] for x in INT_OPS] + [x[0] for x in SHIFT_OPS]
    o.append("// integer operators: add sub mul and or xor shl shr shrun (the long ones have no shifts)")
    o.append("#define VST_INT_OPS %d" % len(int_ops))
    o.append("static const short vst_i32[%d][5][5][3] = {\n\t%s };" % (len(int_ops), arr3("vi", int_ops, "rsm")))
    lops = [x[0] for x in INT_OPS]
    o.append("static const short vst_i64[%d][5][5][3] = {\n\t%s };" % (len(lops), arr3("vl", lops, "rsm")))
    fops = [x[0] for x in FP_OPS]
    o.append("static const short vst_f32[4][5][5][3] = {\n\t%s };" % arr3("vf", fops, "rsm"))
    o.append("static const short vst_f64[4][5][5][3] = {\n\t%s };" % arr3("vd", fops, "rsm"))

    def arr2(fam, ops):
        rows = []
        for op in ops:
            ra = []
            for A in KINDS:
                ra.append("{ %s }" % ", ".join(st(table.get((fam, op, A, B))) for B in KINDS))
            rows.append("{ %s }" % ", ".join(ra))
        return ",\n\t".join(rows)

    icc = [c for c, _ in ICC]
    fcc = ["eq", "ne", "lt", "le", "gt", "ge", "lt_un", "le_un", "gt_un", "ge_un"]
    o.append("// compare and branch: eq ne lt le gt ge  [cond][A][B]")
    o.append("static const short vst_icc[6][5][5] = {\n\t%s };" % arr2("vicc", icc))
    o.append("static const short vst_lcc[6][5][5] = {\n\t%s };" % arr2("vlcc", icc))
    o.append("// float: eq ne lt le gt ge lt_un le_un gt_un ge_un")
    o.append("static const short vst_fcc[10][5][5] = {\n\t%s };" % arr2("vfcc", fcc))
    o.append("static const short vst_dcc[10][5][5] = {\n\t%s };" % arr2("vdcc", fcc))
    jt = ["jt", "jf", "jt8", "jf8"]
    o.append("static const short vst_jt[4][5] = {\n\t%s };" % ",\n\t".join("{ %s }" % ", ".join(st(table.get(("vjt", n, A))) for A in KINDS) for n in jt))
    cv = [c[0] for c in CVT]
    o.append("// conversions: cvtil cvtul cvtif cvtid cvtfd cvtdf  [which][kind of the operand]")
    o.append("static const short vst_cvt[6][5] = {\n\t%s };" % ",\n\t".join("{ %s }" % ", ".join(st(table.get(("vcvt", n, A))) for A in KINDS) for n in cv))
    o.append("// store to a local and push to the memory stack: [class i l f d][kind of the value]")
    o.append("static const short vst_st[4][5] = {\n\t%s };" % ",\n\t".join("{ %s }" % ", ".join(st(table.get(("vst", c, A))) for A in KINDS) for c in "ilfd"))
    o.append("static const short vst_push[4][5] = {\n\t%s };" % ",\n\t".join("{ %s }" % ", ".join(st(table.get(("vpush", c, A))) for A in KINDS) for c in "ilfd"))
    text = "\n".join(o) + "\n"
    check_tables(text)
    return text


def write(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return False
    open(path, "w").write(text)
    return True


if __name__ == "__main__":
    names = [n for n, _ in funcs]
    changed = [write(OUT_C, c_source()), write(OUT_JSON, json.dumps(names)), write(OUT_H, header())]
    print("gen_vstencils: %d stencils%s" % (len(names), "" if any(changed) else " (unchanged)"))
