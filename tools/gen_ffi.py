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
  struct:S a struct passed or returned BY VALUE: S is declared in "structs" (below). bufs:S an array of them (C gets a pointer to the elements: "bufs:const S"
           for a const pointer), refs:S a pointer to one (a ref or out argument).

  "structs": { "Vec2": { "c": "Vec2", "fields": [ {"name": "x", "type": "float"}, {"name": "y", "type": "float"} ] } }
  declares the C layout (\"c\" is the C type, default the name): the generated code takes sizeof and offsetof from the real type, and the runtime compares
  them with the C# struct when the method that calls it is compiled: a struct that differs in size, or in the offset, size or kind of a field, is refused
  with a message (not run with a corrupt stack). Fields are numbers and pointers (int uint short ushort sbyte byte long ulong float double intptr);
  a C# struct is a sequence of fields laid out like C does it. The refs form cannot be checked (a ref is typed as a pointer): bufs and struct are.
  "includes": ["lib.h"] and "include_dirs": ["../include"] say where the struct types and the prototypes come from, if not from "c_files".

  callback:Name  a C function pointer that a C# delegate stands behind, declared in "callbacks":
  "callbacks": { "Compare": { "c": "compare_fn", "ret": "int", "args": ["int", "int"], "slots": 8 } }
  \"c\" is the C type of the pointer (a typedef from a header: int (*compare_fn)(int, int)), \"ret\" and \"args\" the types C calls it with (numbers and
  intptr), \"slots\" how many of them can be in use at once (default 8: one for each call that is passed one and has not returned, and nested calls count).
  C gets a function that runs the delegate, VALID ONLY WHILE THE CALL IT WAS PASSED TO RUNS (a comparator, a visitor: there is nothing to keep, so nothing
  dangles; a null delegate is a NULL pointer). The runtime checks the delegate's signature against this when the method that calls is compiled.

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
    "struct": (None, "S", None), "bufs": (None, "b", "sizeof(void*)"), "refs": (None, "p", "sizeof(void*)"),     # (the C type and size come from the struct)
    "callback": (None, "c", "sizeof(void*)"),                                                                       # (the C type comes from the callback)
}
CALLBACKS = {}                  # name -> {"c": C type, "ret": type or void, "args": [types], "slots": n}
FIELD_TYPES = {"int", "uint", "short", "ushort", "sbyte", "byte", "long", "ulong", "longlong", "ulonglong", "float", "double", "intptr"}
STRUCTS = {}                    # name -> {"c": C type, "fields": [(name, type)]}
# what each kind is read and written as
STACK_C = {"i": "int32_t", "l": "int64_t", "f": "float", "d": "double", "p": "void*", "b": "void*", "s": "void*"}      # (S, a struct, is copied whole)


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
    STRUCTS.clear()
    for name, st in m.get("structs", {}).items():
        fields = []
        for fld in st.get("fields", []):
            if "name" not in fld or fld.get("type") not in FIELD_TYPES:
                fail("struct %s: a field is a name and one of: %s (got %r)" % (name, ", ".join(sorted(FIELD_TYPES)), fld))
            fields.append((fld["name"], fld["type"]))
        if not fields:
            fail("struct %s has no fields" % name)
        STRUCTS[name] = {"c": st.get("c", name), "fields": fields}
    CALLBACKS.clear()
    for name, cb in m.get("callbacks", {}).items():
        for key in ("c", "ret", "args"):
            if key not in cb:
                fail("callback %s has no '%s'" % (name, key))
        for t in [cb["ret"]] + list(cb["args"]):
            if t != "void" and t not in FIELD_TYPES:
                fail("callback %s: a callback takes and returns numbers and intptr (%s), not '%s'" % (name, ", ".join(sorted(FIELD_TYPES)), t))
        if "void" in cb["args"]:
            fail("callback %s: 'void' is not an argument type" % name)
        CALLBACKS[name] = {"c": cb["c"], "ret": cb["ret"], "args": list(cb["args"]), "slots": int(cb.get("slots", 8))}
    funcs = []
    for i, fn in enumerate(m.get("functions", [])):
        for key in ("library", "entry", "ret", "args"):
            if key not in fn:
                fail("function #%d has no '%s'" % (i, key))
        for t in [fn["ret"]] + list(fn["args"]):
            if t != "void" and type_base(t) not in TYPES:
                fail("%s: unknown type '%s' (have: %s)" % (fn["entry"], t, ", ".join(sorted(TYPES))))
            if t != "void" and type_base(t) in ("struct", "bufs", "refs") and struct_name(t) not in STRUCTS:
                fail("%s: '%s' names a struct that is not in \"structs\"" % (fn["entry"], t))
            if t != "void" and type_base(t) == "callback" and t.split(":", 1)[1].strip() not in CALLBACKS:
                fail("%s: '%s' names a callback that is not in \"callbacks\"" % (fn["entry"], t))
        if type_base(fn["ret"]) == "callback":
            fail("%s: a result cannot be a callback" % fn["entry"])
        if type_base(fn["ret"]) in ("buf", "ref", "bufs", "refs"):
            fail("%s: a result cannot be '%s'" % (fn["entry"], type_base(fn["ret"])))
        if "void" in fn["args"]:
            fail("%s: 'void' is not an argument type" % fn["entry"])
        funcs.append(fn)
    return m, files, funcs


def type_base(t):
    return t.split(":", 1)[0]


def kind(t):
    return "v" if t == "void" else TYPES[type_base(t)][1]


def struct_name(t):
    """`struct:Vec2` -> Vec2; `bufs:const Vec2` -> Vec2"""
    n = t.split(":", 1)[1].strip()
    return n[len("const "):].strip() if n.startswith("const ") else n


def is_struct_type(t):
    return t != "void" and type_base(t) in ("struct", "bufs", "refs")


def cb_name(t):
    return t.split(":", 1)[1].strip()


def has_callback(fn):
    return any(type_base(a) == "callback" for a in fn["args"])


def no_struct_by_value(fn):
    return any(type_base(a) == "struct" for a in fn["args"]) or (fn["ret"] != "void" and type_base(fn["ret"]) == "struct")


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


def no_stencil(fn):
    """A call that a native block cannot make with a stencil, which only knows the six integer and eight floating-point argument registers of the
    x86-64 System V convention, and arguments of numbers, pointers and arrays: it goes through its wrapper (an island in a block), which is a call the
    C compiler makes, so any number of arguments and any struct will do."""
    ni = sum(1 for a in fn["args"] if kind(a) in "ilpb")
    nf = sum(1 for a in fn["args"] if kind(a) in "fd")
    return has_string(fn) or ni > len(INT_REGS64) or nf > 8 or no_struct_by_value(fn) or has_callback(fn)


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
    if t != "void" and type_base(t) == "callback":
        return CALLBACKS[cb_name(t)]["c"]
    if is_struct_type(t):
        c = STRUCTS[struct_name(t)]["c"]
        if type_base(t) == "struct":
            return c
        return ("const " if t.split(":", 1)[1].strip().startswith("const ") else "") + c + "*"
    if ":" in t:
        return t.split(":", 1)[1].strip()
    if is_ret and type_base(t) == "cstr":
        return "char*"                       # (a result is freed by the runtime, so it is not const)
    return TYPES[t][0]


def stack_size(t):
    if t == "void":
        return "0"
    if type_base(t) == "struct":
        return "sizeof(%s)" % STRUCTS[struct_name(t)]["c"]
    return TYPES[type_base(t)][2]


def generate(manifest, outdir):
    m, files, funcs = load(manifest)
    os.makedirs(outdir, exist_ok=True)
    out = []
    out.append("// GENERATED by tools/gen_ffi.py from %s. Do not edit." % os.path.basename(manifest))
    out.append('#include "Compat.h"\n#include "Sys.h"\n#include "FFI.h"\n#include "System.Array.h"\n#include <stdint.h>\n#include <stddef.h>\n#include <stdlib.h>\n#include <string.h>\n')
    out.append("// the call stencils load an array's elements at 4 bytes from its start (they only exist on a 64-bit target)")
    out.append('_Static_assert(sizeof(void*) == 4 || (size_t)SystemArray_GetElements((PTR)0) == 4, "the array layout changed: tools/gen_ffi.py");')
    for inc in m.get("includes", []):
        out.append('#include "%s"' % inc)
    out.append("// the functions, as the manifest declares them: a definition that disagrees is an error")
    for fn in funcs:
        args = ", ".join(ctype(a) for a in fn["args"]) or "void"
        out.append("%s %s(%s);" % (ctype(fn["ret"], True), fn["entry"], args))
    out.append("")
    out.append("// the C files")
    for p in files:
        out.append('#include "%s"' % p)
    out.append("")
    out.append("// the C layout of each struct the calls name: checked against the C# struct when a method that uses it is compiled")
    st_index = {}
    for n, (name, st) in enumerate(sorted(STRUCTS.items())):
        c = st["c"]
        st_index[name] = n
        out.append("static const tFFIField ffiFields_%d[] = {" % n)
        for fname, ftype in st["fields"]:
            out.append('\t{ "%s", (U32)offsetof(%s, %s), \'%s\', (U32)sizeof(((%s*)0)->%s) },' % (fname, c, fname, TYPES[ftype][1], c, fname))
        out.append("};")
        out.append('static const tFFIStruct ffiStruct_%d = { "%s", (U32)sizeof(%s), %d, ffiFields_%d };' % (n, name, c, len(st["fields"]), n))
    out.append("")
    cb_index = {}
    if CALLBACKS:
        out.append("// the callbacks: a pool of trampolines for each type, each calling the delegate that its slot holds (FFI_CallbackInvoke)")
    for n, (name, cb) in enumerate(sorted(CALLBACKS.items())):
        cb_index[name] = n
        kinds = "".join(TYPES[a][1] for a in cb["args"])
        rk = "v" if cb["ret"] == "void" else TYPES[cb["ret"]][1]
        rc = "void" if cb["ret"] == "void" else TYPES[cb["ret"]][0]
        params = ", ".join("%s a%d" % (TYPES[a][0], i) for i, a in enumerate(cb["args"])) or "void"
        out.append("static const tFFICallback ffiCb_%d;" % n)
        out.append("static HEAP_PTR ffiCbDg_%d[%d]; static int64_t ffiCbH_%d[%d];" % (n, cb["slots"], n, cb["slots"]))
        for slot in range(cb["slots"]):
            body = ["tFFIValue v[%d];" % max(1, len(cb["args"]))]
            for i, a in enumerate(cb["args"]):
                body.append("v[%d].%s = %sa%d;" % (i, TYPES[a][1], "(void*)" if TYPES[a][1] == "p" else "", i))
            body.append("%sFFI_CallbackInvoke(&ffiCb_%d, %d, v);" % ("" if rk == "v" else "tFFIValue r = ", n, slot))
            if rk != "v":
                body.append("return (%s)r.%s;" % (rc, rk))
            out.append("static %s ffiCbT_%d_%d(%s) { %s }" % (rc, n, slot, params, " ".join(body)))
        out.append("static void *const ffiCbTramp_%d[] = { %s };" % (n, ", ".join("(void*)ffiCbT_%d_%d" % (n, slot) for slot in range(cb["slots"]))))
        out.append('static const tFFICallback ffiCb_%d = { "%s", "%s", \'%s\', %d, ffiCbDg_%d, ffiCbH_%d, ffiCbTramp_%d };' % (n, name, kinds, rk, cb["slots"], n, n, n))
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
            elif kd == "c":
                # a callback: a trampoline from the pool of its type, for the duration of the call
                n_cb = sorted(CALLBACKS).index(cb_name(a))
                out.append("\tHEAP_PTR dg%d_; memcpy(&dg%d_, sp + (%s), sizeof(dg%d_));" % (k, k, o, k))
                out.append("\tint cs%d_ = FFI_CallbackAcquire(&ffiCb_%d, dg%d_);" % (k, n_cb, k))
                call_args.append("(%s)(cs%d_ >= 0 ? ffiCbTramp_%d[cs%d_] : NULL)" % (ctype(a), k, n_cb, k))
                frees.append("\tFFI_CallbackRelease(&ffiCb_%d, cs%d_);" % (n_cb, k))
            elif kd == "S":
                # a struct by value: copied off the evaluation stack into a C struct of the real type (the layouts are checked when the caller is compiled)
                out.append("\t%s st%d_; memcpy(&st%d_, sp + (%s), sizeof(st%d_));" % (ctype(a), k, k, o, k))
                call_args.append("st%d_" % k)
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
        elif kind(fn["ret"]) == "S":
            out.append("\t%s r_ = %s;" % (ctype(fn["ret"]), call))
            out.extend(frees)
            out.append("\tmemcpy(sp, &r_, sizeof(r_));")
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
    for n, fn in enumerate(funcs):
        if any(is_struct_type(a) for a in fn["args"]):
            out.append("static const tFFIStruct *const ffi_as_%d[] = { %s };" % (n, ", ".join(("&ffiStruct_%d" % st_index[struct_name(a)]) if is_struct_type(a) else "NULL" for a in fn["args"])))
    for n, fn in enumerate(funcs):
        if has_callback(fn):
            out.append("static const tFFICallback *const ffi_ac_%d[] = { %s };" % (n, ", ".join(("&ffiCb_%d" % cb_index[cb_name(a)]) if type_base(a) == "callback" else "NULL" for a in fn["args"])))
    out.append("const tFFIEntry ffiTable[] = {")
    for n, fn in enumerate(funcs):
        has_as = any(is_struct_type(a) for a in fn["args"])
        ret_st = ("&ffiStruct_%d" % st_index[struct_name(fn["ret"])]) if is_struct_type(fn["ret"]) else "NULL"
        out.append('\t{ "%s", "%s", (void*)%s, ffi_w_%d, %s, %s, "%s", \'%s\', %s, %s, %s, %s },' % (
            fn["library"], fn["entry"], fn["entry"], n, fn["_total"], stack_size(fn["ret"]),
            "".join(kind(a) for a in fn["args"]), kind(fn["ret"]), ('"ffi_%s"' % fn["_sig"]) if not no_stencil(fn) else "NULL",
            ("ffi_as_%d" % n) if has_as else "NULL", ret_st, ("ffi_ac_%d" % n) if has_callback(fn) else "NULL"))
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
    for key in sorted(k for k, names in sigs.items() if any(not no_stencil(f) for f in funcs if f["entry"] in names)):
        src.append(stencil_source(key))
    with open(os.path.join(outdir, "ffi_stencils.gen.c"), "w") as f:
        f.write("\n".join(src))
    with open(os.path.join(outdir, "ffi_sigs.json"), "w") as f:
        json.dump({"stencil_names": ["fnlo", "fnhi"] + ["ffi_" + k for k in sorted(k for k, names in sigs.items() if any(not no_stencil(f) for f in funcs if f["entry"] in names))], "signatures": sorted(sigs), "functions": [
            {"entry": fn["entry"], "library": fn["library"], "sig": fn["_sig"]} for fn in funcs]}, f, indent=1)
    return m, files, funcs


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    m, files, funcs = generate(sys.argv[1], sys.argv[2])
    print("gen_ffi: %d function(s), %d C file(s)" % (len(funcs), len(files)))
