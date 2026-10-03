#!/usr/bin/env python3
"""Generate native/src/MetaDataLayout.gen.h: how each metadata table row in a .NET assembly maps onto
the C row structs in MetaDataTables.h.

The loader used to describe each row as a string of (source, destination) character pairs, with the
destination a run of fixed 4-byte cells padded with `x*` filler cells so the total matched
sizeof(struct) on a 32-bit build. That cannot describe a 64-bit struct (8-byte aligned pointers,
a bigger struct), and it stored pointers through `(unsigned int)` casts.

This script keeps those strings as the ONE description of the file format (SPEC below, with the
32-bit layout they imply), works out where each struct field is on a 32-bit target (from the struct
declarations and the typedefs; no compiler is involved), and emits for every bound column {source, destination width, offsetof(struct, field)}. The emitted
code uses offsetof(), so each target (wasm32, x86-64, ...) gets its own layout from one table, and on
a 32-bit target generated static asserts pin every offset to the value the old strings implied, so
32-bit behaviour is provably unchanged. The output is a build product; it is regenerated when SPEC or the
structs change:

    python3 tools/gen_metadata_layout.py            # (re)write native/src/MetaDataLayout.gen.h
    python3 tools/gen_metadata_layout.py --check    # fail if the file is out of date
    python3 tools/gen_metadata_layout.py --verify-with-compiler   # also cross-check the layout against gcc -m32

The 32-bit layout is computed here from the typedefs in Types.h (no compiler needed); --verify-with-compiler
compares it with what `gcc -m32` says. build.py runs this itself when the header is missing or older than its
inputs, and the header is not committed (it is in .gitignore).
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "native", "src")
OUT = os.path.join(SRC, "MetaDataLayout.gen.h")

# table id, row struct, and the file-format string: pairs of (source, destination) characters.
#   source: a table id (a plain index into that table), 0-9 : ; < (a coded index, see codedTags in MetaData.c),
#           c s i (8/16/32-bit integers), S G B (string / GUID / blob heap index -> pointer), ^ (RVA -> pointer),
#           m (pointer to this metadata), l (is this the last row), I (the row's own original index),
#           x (no data: nothing read, the value is 0)
#   destination: * (a 32-bit cell), s (16-bit), c (8-bit), x (not stored: the source is read and dropped).
SPEC = [
    (0x00, "tMD_Module",                 "sxS*G*GxGx"),
    (0x01, "tMD_TypeRef",                "x*;*S*S*"),
    (0x02, "tMD_TypeDef",                "x*m*i*S*S*0*\x04*\x06*xclcxcxcx*x*x*x*x*x*x*x*x*x*x*I*x*x*x*x*x*x*x*x*x*x*x*x*"),
    (0x04, "tMD_FieldDef",               "x*m*ssxsS*B*x*x*x*x*I*x*"),
    (0x06, "tMD_MethodDef",              "x*m*^*ssssS*B*\x08*x*x*x*x*x*x*I*x*x*x*"),
    (0x08, "tMD_Param",                  "ssssS*"),
    (0x09, "tMD_InterfaceImpl",          "\x02*0*"),
    (0x0A, "tMD_MemberRef",              "x*5*S*B*"),
    (0x0B, "tMD_Constant",               "ccccxs1*B*"),
    (0x0C, "tMD_CustomAttribute",        "2*:*B*"),
    (0x0E, "tMD_DeclSecurity",           "ssxs4*B*"),
    (0x0F, "tMD_ClassLayout",            "ssxsi*\x02*"),
    (0x11, "tMD_StandAloneSig",          "B*"),
    (0x12, "tMD_EventMap",               "\x02*\x14*"),
    (0x14, "tMD_Event",                  "ssxsS*0*"),
    (0x15, "tMD_PropertyMap",            "\x02*\x17*"),
    (0x17, "tMD_Property",               "ssxsS*B*"),
    (0x18, "tMD_MethodSemantics",        "ssxs\x06*6*"),
    (0x19, "tMD_MethodImpl",             "\x02*7*7*"),
    (0x1A, "tMD_ModuleRef",              "S*"),
    (0x1B, "tMD_TypeSpec",               "x*m*B*"),
    (0x1C, "tMD_ImplMap",                "ssxs8*S*\x1a*"),
    (0x1D, "tMD_FieldRVA",               "^*\x04*"),
    (0x20, "tMD_Assembly",               "i*ssssssssi*B*S*S*"),
    (0x23, "tMD_AssemblyRef",            "ssssssssi*B*S*S*B*"),
    (0x29, "tMD_NestedClass",            "\x02*\x02*"),
    (0x2A, "tMD_GenericParam",           "ssss<*S*"),
    (0x2B, "tMD_MethodSpec",             "x*m*7*B*"),
    (0x2C, "tMD_GenericParamConstraint", "\x2a*0*"),
]

VERIFY = "--verify-with-compiler" in sys.argv

POINTER_SOURCES = set("SGB^m")     # these produce pointers, so they are stored pointer-sized
DST_WIDTH_32 = {"*": 4, "s": 2, "c": 1, "x": 0}


def _declarators(stmt):
    """The members declared by one C statement without its ';': [(type text, array length or None, name)].
    `U16 a, b[2];` declares two; every name shares the first piece's base type."""
    pieces = stmt.split(",")
    first = pieces[0].strip()
    mm = re.match(r"^(.*?)(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*$", first)
    if not mm:
        return []
    base = mm.group(1).strip()
    root = re.sub(r"[\s\*]+$", "", base)           # the type words, without pointer stars
    out = [(base, int(mm.group(3)) if mm.group(3) else None, mm.group(2))]
    for piece in pieces[1:]:
        pm = re.match(r"^\s*(\**)\s*(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*$", piece)
        if pm:
            out.append((root + " " + pm.group(1) if pm.group(1) else root, int(pm.group(3)) if pm.group(3) else None, pm.group(2)))
    return out


def parse_struct(struct):
    """The members of `struct tMD_X_ {...}` in MetaDataTables.h, in order, as dicts {name, type, count, members}.
    Members inside a preprocessor conditional are skipped (they are optional diagnostics, never file-bound), and a
    nested `union { ... } name;` is one member, `name`, with its own members listed."""
    text = open(os.path.join(SRC, "MetaDataTables.h")).read()
    m = re.search(r"struct %s_\s*\{(.*?)\n\};" % struct, text, re.S)
    if not m:
        sys.exit("struct %s_ not found in MetaDataTables.h" % struct)
    fields, depth, nested = [], 0, None
    for line in m.group(1).split("\n"):
        line = re.sub(r"//.*", "", line).strip()
        if nested is not None:           # inside `union { ... } name;`
            if line.startswith("}"):
                mm = re.match(r"\}\s*(\w+)\s*;", line)
                if mm and depth == 0:
                    fields.append({"name": mm.group(1), "type": "union", "count": None, "members": nested})
                nested = None
            elif line.endswith(";"):
                nested.extend((t, c) for t, c, _ in _declarators(line[:-1]))
            continue
        if re.match(r"(union|struct)\s*\{", line):
            nested = []
            continue
        if line.startswith("#if"):
            depth += 1
        elif line.startswith("#endif"):
            depth -= 1
        elif line.startswith("#"):
            continue
        elif depth == 0 and line.endswith(";"):
            for t, c, n in _declarators(line[:-1]):
                fields.append({"name": n, "type": t, "count": c, "members": None})
    return fields


def struct_fields(struct):
    """The member names of a row struct in order. Also records each member's declared type in
    struct_fields.decls (the pointer-column check uses it)."""
    fields = parse_struct(struct)
    struct_fields.decls[struct] = dict((f["name"], f["type"]) for f in fields)
    return [f["name"] for f in fields]


struct_fields.decls = {}

# Declared types that hold a pointer (or something the loader may store a pointer into)
POINTER_TYPES = {"STRING", "STRING2", "BLOB_", "PTR", "HEAP_PTR", "GUID_"}


def is_pointer_type(decl):
    return "*" in decl or decl.strip().split()[-1:] and decl.strip().split()[-1] in POINTER_TYPES


# ---- the 32-bit layout, computed without a compiler ---------------------------------------------------------
# (size, alignment) of the C types a row struct is made of, for a 32-bit target (ILP32, i386 System V: a 64-bit
# integer inside a struct is 4-byte aligned). Named types such as U8 or STRING are read from Types.h.
C_BASE = {"char": (1, 1), "unsigned char": (1, 1), "short": (2, 2), "unsigned short": (2, 2),
          "int": (4, 4), "unsigned int": (4, 4), "unsigned": (4, 4), "long": (4, 4), "unsigned long": (4, 4),
          "long long": (8, 4), "unsigned long long": (8, 4), "float": (4, 4), "double": (8, 4)}


def typedefs():
    td = {}
    for name in ("Types.h", "Compat.h"):
        path = os.path.join(SRC, name)
        if os.path.exists(path):
            for line in open(path).read().split("\n"):
                m = re.match(r"\s*typedef\s+(.+?)\s*(\w+)\s*;", re.sub(r"//.*", "", line))
                if m:
                    td[m.group(2)] = m.group(1)
    return td


def type_info(t, td):
    t = re.sub(r"\b(const|volatile|struct|enum)\b", "", t).strip()
    if "*" in t:
        return (4, 4)
    t = " ".join(t.split())
    if t in C_BASE:
        return C_BASE[t]
    if t in td:
        return type_info(td[t], td)
    sys.exit("don't know the size of type `%s`: add it to C_BASE, or make sure its typedef is in Types.h" % t)


def layout_32bit():
    """For every row struct: sizeof, and [(offset, name, size)] of its members on a 32-bit target."""
    td = typedefs()
    sizes, fields = {}, {}
    for _, struct, _ in SPEC:
        off, maxalign, out = 0, 1, []
        struct_fields(struct)       # also records each member's declared type, for the pointer-column check
        for f in parse_struct(struct):
            if f["type"] == "union":
                size = max((type_info(t, td)[0] * (c or 1) for t, c in f["members"]), default=0)
                align = max((type_info(t, td)[1] for t, c in f["members"]), default=1)
            else:
                size, align = type_info(f["type"], td)
                size *= f["count"] or 1
            off = (off + align - 1) & ~(align - 1)
            out.append((off, f["name"], size))
            off += size
            maxalign = max(maxalign, align)
        sizes[struct] = (off + maxalign - 1) & ~(maxalign - 1)
        fields[struct] = out
    return sizes, fields


def probe_with_compiler():
    """Ask gcc -m32 for the real offset and size of every field of every row struct, and the struct sizes.
    Only used to cross-check layout_32bit() (--verify-with-compiler); the generator does not need a compiler."""
    lines = ['#include <stdio.h>', '#include <stddef.h>', '#include "Compat.h"', '#include "Sys.h"',
             '#include "MetaData.h"', '#include "MetaDataTables.h"', 'int main(void) {']
    for _, struct, _ in SPEC:
        lines.append('printf("S %s %%u\\n", (unsigned)sizeof(%s));' % (struct, struct))
        for f in struct_fields(struct):
            lines.append('printf("F %s %s %%u %%u\\n", (unsigned)offsetof(%s, %s), (unsigned)sizeof(((%s*)0)->%s));'
                         % (struct, f, struct, f, struct, f))
    lines += ['return 0; }']
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "p.c"), os.path.join(d, "p")
        open(c, "w").write("\n".join(lines))
        cmd = ["gcc", "-m32", "-std=gnu99", "-I", SRC, "-idirafter", "/usr/include/x86_64-linux-gnu", c, "-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit("probe compile failed (needs gcc with 32-bit support):\n" + r.stderr[:2000])
        out = subprocess.run([exe], capture_output=True, text=True).stdout
    sizes, fields = {}, {}
    for l in out.splitlines():
        p = l.split()
        if p[0] == "S":
            sizes[p[1]] = int(p[2])
        else:
            fields.setdefault(p[1], []).append((int(p[3]), p[2], int(p[4])))   # offset, name, size
    return sizes, fields


def verify_against_compiler(sizes, fields):
    """Compare the computed 32-bit layout with gcc -m32's. Skipped (with a note) if there is no 32-bit compiler."""
    try:
        psizes, pfields = probe_with_compiler()
    except SystemExit as e:
        print("note: compiler cross-check skipped: %s" % str(e).splitlines()[0], file=sys.stderr)
        return
    for struct in sizes:
        if psizes[struct] != sizes[struct]:
            sys.exit("layout mismatch: sizeof(%s) is %d by the compiler, %d computed" % (struct, psizes[struct], sizes[struct]))
        got = sorted(pfields[struct])
        want = sorted(fields[struct])
        if got != want:
            sys.exit("layout mismatch in %s:\n  compiler: %s\n  computed: %s" % (struct, got, want))
    print("layout cross-check against gcc -m32: %d structs agree" % len(sizes), file=sys.stderr)


def generate():
    sizes, fields = layout_32bit()
    if VERIFY:
        verify_against_compiler(sizes, fields)
    out = []
    w = out.append
    w("// GENERATED by tools/gen_metadata_layout.py from its SPEC table and MetaDataTables.h. Do not edit:")
    w("// change SPEC or the structs and regenerate. See that script for what this is and why.")
    w("")
    w("#if !defined(__METADATALAYOUT_GEN_H)")
    w("#define __METADATALAYOUT_GEN_H")
    w("")
    w("#include <stddef.h>")
    w("#include <stdint.h>")
    w("")
    w("// Destination widths. MDDST_PTR is pointer-sized (4 bytes on a 32-bit target, 8 on a 64-bit one).")
    w("#define MDDST_NONE 0   // read from the file and dropped")
    w("#define MDDST_U8   1")
    w("#define MDDST_U16  2")
    w("#define MDDST_U32  4")
    w("#define MDDST_PTR  8")
    w("typedef struct { unsigned char src; unsigned char dst; unsigned short ofs; } tMDCol;")
    w("typedef struct { const tMDCol *pCols; unsigned short numCols; unsigned int rowSize; } tMDTableLayout;")
    w("")
    w("#define MD_LAYOUT_ASSERT(name, cond) typedef char md_layout_assert_##name[(cond) ? 1 : -1]")
    w("")
    asserts, tables, partial = [], {}, []
    for tid, struct, spec in SPEC:
        pairs = [(spec[i], spec[i + 1]) for i in range(0, len(spec), 2)]
        ofs32, cols = 0, []
        for src, dst in pairs:
            if dst not in DST_WIDTH_32:
                sys.exit("table 0x%02X: bad destination %r" % (tid, dst))
            width32 = DST_WIDTH_32[dst]
            if src != "x":
                if width32 == 0:
                    cols.append((src, "MDDST_NONE", None))          # read and dropped
                else:
                    cands = [f for f in fields[struct] if f[0] == ofs32]
                    if not cands:
                        sys.exit("table 0x%02X (%s): the spec column %r at 32-bit offset %d has no struct field there"
                                 % (tid, struct, src, ofs32))
                    off, name, size = cands[0]
                    if size < width32:
                        sys.exit("table 0x%02X (%s): field %s is %d bytes but the spec cell is %d" % (tid, struct, name, size, width32))
                    if size > width32:
                        # e.g. a padding byte of the file format stored into the first byte of `U8 padding0[3]`.
                        # The old loader did exactly that; reported so it is never silent.
                        partial.append("%s.%s: a %d-byte cell into the start of a %d-byte field" % (struct, name, width32, size))
                    if src in POINTER_SOURCES:
                        if width32 != 4:
                            sys.exit("table 0x%02X: pointer column with a %d-byte cell" % (tid, width32))
                        # The loader stores a pointer here, so the field must be pointer-typed: a 4-byte
                        # integer field that happens to hold a pointer on a 32-bit target is a bug on 64-bit.
                        decl = struct_fields.decls[struct].get(name, "")
                        if not is_pointer_type(decl):
                            sys.exit("table 0x%02X (%s): column %r stores a pointer into field %s, declared as `%s` -- "
                                     "declare it as a pointer type (e.g. PTR)" % (tid, struct, src, name, decl.strip()))
                        kind = "MDDST_PTR"
                    else:
                        kind = {4: "MDDST_U32", 2: "MDDST_U16", 1: "MDDST_U8"}[width32]
                    cols.append((src, kind, name))
                    asserts.append((struct, name, ofs32))
            ofs32 += width32
        spec_total = ofs32
        if spec_total != sizes[struct]:
            sys.exit("table 0x%02X (%s): the spec lays out %d bytes but sizeof(struct) is %d on 32-bit" % (tid, struct, spec_total, sizes[struct]))
        tables[tid] = (struct, cols, sizes[struct])
        w("static const tMDCol mdCols_%02X[] = {  // %s" % (tid, struct))
        for src, kind, name in cols:
            sc = ("'%s'" % src) if (32 <= ord(src) < 127 and src not in "'\\") else "0x%02X" % ord(src)
            w("    { %-5s %-11s %s }," % (sc + ",", kind + ",", ("(unsigned short)offsetof(%s, %s)" % (struct, name)) if name else "0"))
        w("};")
    w("")
    w("static const tMDTableLayout mdTableLayouts[MAX_TABLES] = {")
    for tid, (struct, cols, size) in sorted(tables.items()):
        w("    [0x%02X] = { mdCols_%02X, %d, sizeof(%s) }," % (tid, tid, len(cols), struct))
    w("};")
    w("")
    w("// On a 32-bit target the layout must be exactly the one the old format strings described.")
    w("#if UINTPTR_MAX == 0xFFFFFFFFu")
    for i, (struct, name, ofs) in enumerate(asserts):
        w("MD_LAYOUT_ASSERT(o%d_%s_%s, offsetof(%s, %s) == %d);" % (i, struct, name, struct, name, ofs))
    for tid, (struct, cols, size) in sorted(tables.items()):
        w("MD_LAYOUT_ASSERT(s_%s, sizeof(%s) == %d);" % (struct, struct, size))
    w("#endif")
    w("")
    w("#endif")
    for note in partial:
        print("note: " + note, file=sys.stderr)
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    text = generate()
    if "--check" in sys.argv:
        if not os.path.exists(OUT) or open(OUT).read() != text:
            sys.exit("MetaDataLayout.gen.h is out of date: run tools/gen_metadata_layout.py")
        print("MetaDataLayout.gen.h is up to date")
    else:
        # Only touch the file if its content changes, so a build that regenerates it does not recompile everything
        if os.path.exists(OUT) and open(OUT).read() == text:
            print("unchanged", os.path.relpath(OUT))
        else:
            open(OUT, "w").write(text)
            print("wrote", os.path.relpath(OUT), "(%d tables, %d bound columns)" % (len(SPEC), text.count("{ '") + text.count("{ 0x")))
