#!/usr/bin/env python3
"""Generate the FFI table from a manifest: tools/gen_ffi.py MANIFEST.json OUTDIR

The manifest names the C files that go into the runtime and the functions a program may call with [DllImport]:

    {
      "c_files":  ["mylib.c"],                     // paths relative to the manifest
      "cflags":   ["-O2"],                         // optional, for the unit that contains them
      "functions": [
        {"library": "mylib", "entry": "add_numbers", "ret": "int", "args": ["int", "int"]}
      ]
    }

Types: int uint short ushort sbyte byte long (int64_t) ulong (uint64_t) longlong ulonglong float double void (a result only), and
  intptr   a pointer-sized value passed as it is: IntPtr, a `ref` or `out` argument (the managed pointer: nothing moves, so no pinning), an
           unsafe pointer. C type void* unless given: "intptr:int32_t*" or "ref:int32_t*"
  buf      a C# array (of a blittable type): C gets a pointer to its first element, null for null. Zero copy. "buf:const double*"
  cstr     a C# string: C gets a temporary NUL-terminated UTF-8 copy for the duration of the call (const char*, or "cstr:char*").
           As a result: a char* that the runtime turns into a string and then frees with free(), as .NET does.

Writes OUTDIR/FFI.gen.c: a prototype for each function (from the manifest), then the C files themselves (#included, so a signature that
disagrees with a definition is a compile error), a wrapper for each function that reads its arguments off the evaluation stack and calls it,
and the table (see native/src/FFI.h). Also OUTDIR/ffi_sigs.json: the distinct signatures, for the stencil generator.
"""
import json
import os
import sys

# manifest type -> (C type, kind letter, size on the evaluation stack as a C expression)
TYPES = {
    "int": ("int32_t", "i", "4"), "uint": ("uint32_t", "i", "4"),
    "short": ("int16_t", "i", "4"), "ushort": ("uint16_t", "i", "4"),
    "sbyte": ("int8_t", "i", "4"), "byte": ("uint8_t", "i", "4"),
    "long": ("int64_t", "l", "8"), "ulong": ("uint64_t", "l", "8"),
    "longlong": ("long long", "l", "8"), "ulonglong": ("unsigned long long", "l", "8"),
    "float": ("float", "f", "4"), "double": ("double", "d", "8"),
    "intptr": ("void*", "p", "sizeof(void*)"), "ref": ("void*", "p", "sizeof(void*)"),
    "buf": ("void*", "b", "sizeof(void*)"), "cstr": ("const char*", "s", "sizeof(void*)"),
}
# what each kind is read and written as
STACK_C = {"i": "int32_t", "l": "int64_t", "f": "float", "d": "double", "p": "void*", "b": "void*", "s": "void*"}


def fail(msg):
    print("gen_ffi: error: " + msg, file=sys.stderr)
    sys.exit(1)


def load(path):
    with open(path) as f:
        text = f.read()
    # allow // comments in the manifest, which JSON itself does not
    text = "\n".join(l.split("//")[0] if "//" in l and not '"' in l.split("//")[1] else l for l in text.splitlines())
    try:
        m = json.loads(text)
    except ValueError as e:
        fail("%s is not valid JSON: %s" % (path, e))
    base = os.path.dirname(os.path.abspath(path))
    files = []
    for c in m.get("c_files", []):
        p = c if os.path.isabs(c) else os.path.join(base, c)
        if not os.path.exists(p):
            fail("C file not found: %s" % p)
        files.append(p)
    funcs = []
    for i, fn in enumerate(m.get("functions", [])):
        for key in ("library", "entry", "ret", "args"):
            if key not in fn:
                fail("function #%d has no '%s'" % (i, key))
        for t in [fn["ret"]] + list(fn["args"]):
            if t != "void" and type_base(t) not in TYPES:
                fail("%s: unknown type '%s' (have: %s)" % (fn["entry"], t, ", ".join(sorted(TYPES))))
        if type_base(fn["ret"]) in ("buf", "ref"):
            fail("%s: a result cannot be '%s'" % (fn["entry"], type_base(fn["ret"])))
        if "void" in fn["args"]:
            fail("%s: 'void' is not an argument type" % fn["entry"])
        if len(fn["args"]) > 6:
            fail("%s: at most 6 arguments are supported" % fn["entry"])
        funcs.append(fn)
    return m, files, funcs


def type_base(t):
    return t.split(":", 1)[0]


def kind(t):
    return "v" if t == "void" else TYPES[type_base(t)][1]


# What the stencil does with the result in rax: the C callee leaves the bits above a narrow result undefined, so they are made right here
NARROW_RET = {"sbyte": "b", "byte": "B", "short": "s", "ushort": "S"}      # (as a result; b and s are also argument kinds, which is a different thing)


def ret_code(t):
    return NARROW_RET.get(t) or ("T" if kind(t) == "s" else kind(t))      # (T: a string result)


def sig_key(fn):
    return "".join(kind(a) for a in fn["args"]) + "_" + ret_code(fn["ret"])


INT_REGS64 = ["rdi", "rsi", "rdx", "rcx", "r8", "r9"]
INT_REGS32 = ["edi", "esi", "edx", "ecx", "r8d", "r9d"]
STACK_BYTES = {"i": 4, "l": 8, "f": 4, "d": 8, "p": 8, "b": 8, "s": 8}      # (stencils are x86-64: a pointer is 8 bytes)


def has_string(fn):
    return any(kind(a) == "s" for a in fn["args"]) or kind(fn["ret"]) == "s"


def stencil_source(key):
    """A stencil that calls the C function whose address is in r11 (put there by st_fnlo and st_fnhi), with the arguments where an
    evaluation stack of this signature has them (r12 is the stack pointer) and the result where the first of them was."""
    args, ret = key.split("_")
    total = sum(STACK_BYTES[a] for a in args)
    lines, ofs, ni, nf = [], 0, 0, 0
    for a in args:
        d = ofs - total
        if a in "ilpb":
            if ni >= len(INT_REGS64):
                raise SystemExit("gen_ffi: too many integer arguments in signature " + key)
            if a == "i":
                lines.append("movl %d(%%r12), %%%s" % (d, INT_REGS32[ni]))
            else:
                lines.append("movq %d(%%r12), %%%s" % (d, INT_REGS64[ni]))
            if a == "b":
                # an array: the pointer to its elements (4 bytes in), null staying null, without a jump (a stencil has none)
                lines += ["leaq 4(%%%s), %%rax" % INT_REGS64[ni], "testq %%%s, %%%s" % (INT_REGS64[ni], INT_REGS64[ni]), "cmovneq %%rax, %%%s" % INT_REGS64[ni]]
            ni += 1
        else:
            if nf >= 8:
                raise SystemExit("gen_ffi: too many floating-point arguments in signature " + key)
            lines.append("%s %d(%%r12), %%xmm%d" % ("movss" if a == "f" else "movsd", d, nf))
            nf += 1
        ofs += STACK_BYTES[a]
    lines.append("movl $%d, %%eax" % nf)                       # (the number of vector registers in use: only a variadic callee reads it)
    # the stack must be 16-byte aligned at the call, and a native block is entered with it however it is: remember it in rbx, which
    # is callee-saved so the C function keeps it (and is put back, so the block's caller sees it unchanged)
    lines += ["pushq %rbx", "movq %rsp, %rbx", "andq $-16, %rsp", "call *%r11", "movq %rbx, %rsp", "popq %rbx"]
    r = -total
    if ret == "i":
        lines.append("movl %%eax, %d(%%r12)" % r)
    elif ret in "bBsS":
        lines.append({"b": "movsbl %al, %eax", "B": "movzbl %al, %eax", "s": "movswl %ax, %eax", "S": "movzwl %ax, %eax"}[ret])
        lines.append("movl %%eax, %d(%%r12)" % r)
    elif ret in "lp":
        lines.append("movq %%rax, %d(%%r12)" % r)
    elif ret == "f":
        lines.append("movss %%xmm0, %d(%%r12)" % r)
    elif ret == "d":
        lines.append("movsd %%xmm0, %d(%%r12)" % r)
    ret_bytes = 0 if ret == "v" else (4 if ret in "ibBsSf" else 8)
    if ret_bytes != total:
        lines.append("leaq %d(%%r12), %%r12" % (ret_bytes - total))
    lines.append("ret")
    body = "\n".join('\t"%s\\n"' % l for l in lines)
    return "__attribute__((naked)) void st_ffi_%s(void) {\n\t__asm__(\n%s);\n}\n" % (key, body)


def ctype(t, is_ret=False):
    if t == "void":
        return "void"
    if ":" in t:
        return t.split(":", 1)[1].strip()
    if is_ret and type_base(t) == "cstr":
        return "char*"                       # (a result is freed by the runtime, so it is not const)
    return TYPES[t][0]


def stack_size(t):
    return "0" if t == "void" else TYPES[type_base(t)][2]


def generate(manifest, outdir):
    m, files, funcs = load(manifest)
    os.makedirs(outdir, exist_ok=True)
    out = []
    out.append("// GENERATED by tools/gen_ffi.py from %s. Do not edit." % os.path.basename(manifest))
    out.append('#include "Compat.h"\n#include "Sys.h"\n#include "FFI.h"\n#include "System.Array.h"\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n')
    out.append("// the call stencils load an array's elements at 4 bytes from its start (they only exist on a 64-bit target)")
    out.append('_Static_assert(sizeof(void*) == 4 || (size_t)SystemArray_GetElements((PTR)0) == 4, "the array layout changed: tools/gen_ffi.py");')
    out.append("// the functions, as the manifest declares them: a definition that disagrees is an error")
    for fn in funcs:
        args = ", ".join(ctype(a) for a in fn["args"]) or "void"
        out.append("%s %s(%s);" % (ctype(fn["ret"], True), fn["entry"], args))
    out.append("")
    out.append("// the C files")
    for p in files:
        out.append('#include "%s"' % p)
    out.append("")
    sigs = {}
    out.append("// each reads its arguments off the evaluation stack and writes the result where the first one was")
    for n, fn in enumerate(funcs):
        offs, parts = [], []
        for a in fn["args"]:
            offs.append(" + ".join(parts) if parts else "0")
            parts.append(stack_size(a))
        total = " + ".join(parts) if parts else "0"
        out.append("static void ffi_w_%d(PTR sp) {" % n)
        call_args, frees = [], []
        for k, (a, o) in enumerate(zip(fn["args"], offs)):
            kd = kind(a)
            if kd == "b":
                # an array: a pointer to its elements (null stays null). Nothing moves in this runtime, so it needs no pinning
                out.append("\tvoid *h%d_; memcpy(&h%d_, sp + (%s), sizeof(h%d_));" % (k, k, o, k))
                call_args.append("(%s)(h%d_ ? (void*)SystemArray_GetElements((PTR)h%d_) : NULL)" % (ctype(a), k, k))
            elif kd == "s":
                # a string: a temporary NUL-terminated UTF-8 copy, freed after the call if it did not fit in the buffer on the stack
                out.append("\tvoid *h%d_; char t%d_[256]; int m%d_ = 0; memcpy(&h%d_, sp + (%s), sizeof(h%d_));" % (k, k, k, k, o, k))
                out.append("\tconst char *s%d_ = FFI_StringToUtf8((HEAP_PTR)h%d_, t%d_, sizeof(t%d_), &m%d_);" % (k, k, k, k, k))
                call_args.append("(%s)s%d_" % (ctype(a), k))
                frees.append("\tif (m%d_) { free((void*)s%d_); }" % (k, k))
            else:
                sc = STACK_C[kd]
                call_args.append("(%s)({ %s v_; memcpy(&v_, sp + (%s), sizeof(v_)); v_; })" % (ctype(a), sc, o))
        call = "%s(%s)" % (fn["entry"], ", ".join(call_args))
        if fn["ret"] == "void":
            out.append("\t%s;" % call)
            out.extend(frees)
        elif kind(fn["ret"]) == "s":
            # a string result: made into a string, and the char* freed (as .NET does with a string result)
            out.append("\tchar *rs_ = (char*)%s;" % call)
            out.extend(frees)
            out.append("\tvoid *r_ = rs_ ? (void*)FFI_StringFromUtf8(rs_) : NULL;")
            out.append("\tif (rs_) { free(rs_); }")
            out.append("\tmemcpy(sp, &r_, sizeof(r_));")
        else:
            sc = STACK_C[kind(fn["ret"])]
            out.append("\t%s r_ = (%s)%s;" % (sc, sc, call))
            out.extend(frees)
            out.append("\tmemcpy(sp, &r_, sizeof(r_));")
        out.append("}")
        key = sig_key(fn)
        sigs.setdefault(key, []).append(fn["entry"])
        fn["_sig"] = key
        fn["_total"] = total
    out.append("")
    out.append("const tFFIEntry ffiTable[] = {")
    for n, fn in enumerate(funcs):
        out.append('\t{ "%s", "%s", (void*)%s, ffi_w_%d, %s, %s, "%s", \'%s\', %s },' % (
            fn["library"], fn["entry"], fn["entry"], n, fn["_total"], stack_size(fn["ret"]),
            "".join(kind(a) for a in fn["args"]), kind(fn["ret"]), ('"ffi_%s"' % fn["_sig"]) if not has_string(fn) else "NULL"))
    if not funcs:
        out.append('\t{ NULL, NULL, NULL, NULL, 0, 0, NULL, \'v\', NULL }')
    out.append("};")
    out.append("const U32 ffiCount = %d;" % len(funcs))
    with open(os.path.join(outdir, "FFI.gen.c"), "w") as f:
        f.write("\n".join(out) + "\n")
    src = ["// GENERATED by tools/gen_ffi.py. Do not edit.",
           "// The address of the C function, put in r11 by two stencils (a 32-bit hole each) before the one that calls it",
           "void st_fnlo(void) { __asm__ volatile(\"mov $HOLE0, %%r11d\" ::: \"r11\"); }",
           "void st_fnhi(void) { __asm__ volatile(\"mov $HOLE0, %%eax\\n\\tshl $32, %%rax\\n\\tor %%rax, %%r11\" ::: \"rax\", \"r11\", \"cc\"); }", ""]
    for key in sorted(k for k, names in sigs.items() if any(not has_string(f) for f in funcs if f["entry"] in names)):
        src.append(stencil_source(key))
    with open(os.path.join(outdir, "ffi_stencils.gen.c"), "w") as f:
        f.write("\n".join(src))
    with open(os.path.join(outdir, "ffi_sigs.json"), "w") as f:
        json.dump({"stencil_names": ["fnlo", "fnhi"] + ["ffi_" + k for k in sorted(k for k, names in sigs.items() if any(not has_string(f) for f in funcs if f["entry"] in names))], "signatures": sorted(sigs), "functions": [
            {"entry": fn["entry"], "library": fn["library"], "sig": fn["_sig"]} for fn in funcs]}, f, indent=1)
    return m, files, funcs


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    m, files, funcs = generate(sys.argv[1], sys.argv[2])
    print("gen_ffi: %d function(s), %d C file(s)" % (len(funcs), len(files)))
