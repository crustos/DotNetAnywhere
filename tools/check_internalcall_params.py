#!/usr/bin/env python3
"""Lint the native (internal-call) functions' reads of their parameters.

A native method gets a block of memory holding its arguments one after another, each taking its
evaluation-stack slot: 4 bytes for an int, bool, char, float or enum, 8 for a long or double, and a pointer's
width for a reference, array, string or IntPtr. Natives read them at hand-written offsets, e.g.
`((U32*)pParams)[1]` ("the second 4-byte word"), which silently assumes every earlier argument is 4 bytes.
That is true on a 32-bit target and wrong on a 64-bit one when an earlier argument is a reference.

For every native registered in InternalCall.c this computes where each argument really is, for both pointer
sizes, works out which argument each read was written for (the one at that offset with 4-byte pointers),
and reports any read that is not at that argument's real 64-bit offset. It also reports natives that return
a reference through a 32-bit store. Exit status 1 if anything is wrong.

    python3 tools/check_internalcall_params.py          # check
    python3 tools/check_internalcall_params.py -v       # also list every read it understood
    python3 tools/check_internalcall_params.py --fix    # rewrite the wrong reads (then re-check)

Offsets are written with PSZ, the size of a pointer (see Sys.h): `INTERNALCALL_PARAM(PSZ + 4, U32)` is the
argument after one pointer and one int. The checker evaluates such expressions for both pointer sizes.
"""
import os, re, sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native", "src")
VERBOSE = "-v" in sys.argv
FIX = "--fix" in sys.argv

FOUR = {"INT32", "UINT32", "SINGLE", "BOOLEAN", "CHAR", "BYTE", "SBYTE", "INT16", "UINT16",
        "IO_FILESYSTEMATTRIBUTES", "IO_FILESHARE", "IO_FILEMODE", "IO_FILEACCESS", "PLATFORMID",
        "GLOBALIZATION_UNICODECATEGORY"}
EIGHT = {"INT64", "UINT64", "DOUBLE"}
# anything else (OBJECT, STRING, TYPE, INTPTR, ARRAY_*, RUNTIMETYPE, delegates, handles ...) is a pointer


def kind(tname):
    n = tname.replace("TYPE_SYSTEM_", "")
    if n in FOUR: return "4"
    if n in EIGHT: return "8"
    return "P"


def size(k, ptr):
    return {"4": 4, "8": 8, "P": ptr}[k]


def read_table():
    """C function name -> (return kind, [argument kinds])"""
    text = open(os.path.join(SRC, "InternalCall.c")).read()
    table = {}
    for m in re.finditer(r'\{\s*(?:NULL|"[^"]*")\s*,\s*(?:NULL|"[^"]*")\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(TYPE_SYSTEM_\w+)\s*,\s*(\d+)\s*(?:,\s*\{([^}]*)\})?\s*\}', text):
        name, fn, ret, n, args = m.group(1), m.group(2), m.group(3), int(m.group(4)), m.group(5)
        kinds = [kind(a.strip()) for a in args.split(",")] if args else []
        if len(kinds) != n:
            print("warning: %s declares %d args but lists %d" % (fn, n, len(kinds)))
        table[fn] = (kind(ret), ret, kinds)
    return table


def elem_size(ctype, ptr):
    """size of the element type in an expression like ((T*)pParams)[i]"""
    t = ctype.strip()
    if t.count("*") >= 1 and not t.startswith("U32") and not t.startswith("I32"):
        return ptr
    base = t.replace("*", "").strip()
    if base in ("U32", "I32", "int", "unsigned int", "float", "BOOL", "U16", "I16", "U8", "I8"): return 4
    if base in ("U64", "I64", "double"): return 8
    return ptr   # HEAP_PTR, PTR, tSystemArray, tMD_..., void ... are all pointer-typed


def eval_ofs(expr, ptr):
    """Evaluate an offset like `2*PSZ+4` or `sizeof(void*)` for a pointer size; None if it is not simple arithmetic."""
    e = expr.replace("sizeof(void*)", "PSZ").replace("sizeof(PTR)", "PSZ").replace("PSZ", str(ptr))
    if re.fullmatch(r"[\d\s\+\*\(\)]+", e):
        return eval(e)
    return None


def expr_for(kinds, k):
    """The offset of argument k as a readable expression in PSZ"""
    nP = sum(1 for x in kinds[:k] if x == "P")
    c = sum(size(x, 0) for x in kinds[:k])      # the fixed part (pointers count 0 here)
    if nP == 0:
        return str(c)
    base = "PSZ" if nP == 1 else "%d*PSZ" % nP
    return base + ("+%d" % c if c else "")


def reads_in(body):
    """(description, 32-bit offset, 64-bit offset, element/declared type) for every read of pParams"""
    out = []
    for m in re.finditer(r'\(\(\s*([\w\s\*]+?)\s*\*\s*\)\s*pParams\s*\)\s*\[\s*(\d+)\s*\]', body):
        t, i = m.group(1) + "*", int(m.group(2))
        # `(T**)pParams)[i]`: T* elements -> pointer sized; `(U32*)pParams)[i]` -> 4
        out.append((m.group(0), i * elem_size(t, 4), i * elem_size(t, 8), t))
    for m in re.finditer(r'INTERNALCALL_PARAM\s*\(\s*([^,]+?)\s*,\s*([\w\s\*]+?)\s*\)', body):
        o32, o64 = eval_ofs(m.group(1), 4), eval_ofs(m.group(1), 8)
        if o32 is None:
            continue
        out.append((m.group(0), o32, o64, m.group(2)))
    return out


def main():
    table = read_table()
    bad = 0
    seen = 0
    fixes = {}      # file -> [(old text of the whole function body, new text)]
    for fname in sorted(os.listdir(SRC)):
        if not fname.endswith(".c"):
            continue
        text = open(os.path.join(SRC, fname)).read()
        for m in re.finditer(r'tAsyncCall\*\s+(\w+)\s*\(\s*PTR pThis_\s*,\s*PTR pParams\s*,\s*PTR pReturnValue\s*\)\s*\{', text):
            fn = m.group(1)
            if fn not in table:
                continue
            # body: up to the matching closing brace
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += (text[i] == "{") - (text[i] == "}")
                i += 1
            body = text[m.end():i]
            retk, retname, kinds = table[fn]
            new_body = body
            exp32, exp64, o32, o64 = [], [], 0, 0
            for k in kinds:
                exp32.append(o32); exp64.append(o64)
                o32 += size(k, 4); o64 += size(k, 8)
            for desc, ofs32, ofs64, t in reads_in(body):
                seen += 1
                # which argument was this read written for? the one at that offset on a 32-bit target
                if ofs32 not in exp32:
                    print("%s:%s: %s reads offset %d, which is not the start of any argument (args at %s)"
                          % (fname, fn, desc, ofs32, exp32))
                    bad += 1
                    continue
                k = exp32.index(ofs32)
                if ofs64 is None:
                    # INTERNALCALL_PARAM(literal offset): a literal is the same on both targets
                    ofs64 = ofs32
                if ofs64 != exp64[k]:
                    print("%s:%s: %s is meant for argument %d (a %s) but reads offset %d on 64-bit; it is at %d"
                          % (fname, fn, desc, k, {"4": "4-byte value", "8": "8-byte value", "P": "pointer"}[kinds[k]], ofs64, exp64[k]))
                    bad += 1
                    # the replacement: INTERNALCALL_PARAM(<expr>, <type>), where <type> is what the read used
                    mi = re.match(r'\(\(\s*([\w\s\*]+?)\s*\*\s*\)\s*pParams', desc)
                    if mi:
                        rtype = mi.group(1).strip()          # ((T**)pParams)[i] reads a T*, ((U32*)pParams)[i] a U32
                        if not rtype.endswith("*") and rtype not in ("U32", "I32", "float", "int", "U64", "I64", "double"):
                            rtype = rtype
                        elem = rtype
                    else:
                        mi = re.match(r'INTERNALCALL_PARAM\s*\(\s*[^,]+,\s*([\w\s\*]+?)\s*\)', desc)
                        elem = mi.group(1).strip()
                    new = "INTERNALCALL_PARAM(%s, %s)" % (expr_for(kinds, k), elem)
                    new_body = new_body.replace(desc, new)
                elif VERBOSE:
                    print("ok  %s:%s argument %d: %s" % (fname, fn, k, desc))
            if FIX and new_body != body:
                fixes.setdefault(fname, []).append((body, new_body))
            # returning a reference through a 32-bit store
            if retk == "P" and re.search(r'\*\s*\(\s*(U32|I32|unsigned int|int)\s*\*\s*\)\s*pReturnValue', body):
                print("%s:%s returns %s (a pointer) through a 32-bit store" % (fname, fn, retname))
                bad += 1
    if FIX:
        for fname, repls in fixes.items():
            path = os.path.join(SRC, fname)
            text = open(path).read()
            for old, new in repls:
                assert text.count(old) == 1, (fname, "function body not unique")
                text = text.replace(old, new, 1)
            open(path, "w").write(text)
            print("rewrote %d function(s) in %s" % (len(repls), fname))
    print("%d reads checked, %d problems" % (seen, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
