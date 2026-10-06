// Copyright (c) 2012 DotNetAnywhere
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// WasmJIT: a method's CIL compiled to a WebAssembly function at run time. See WasmJIT.h for the contract.
//
// The pieces:
//  1. decode the CIL into Ins records (rejecting anything outside the supported subset),
//  2. split it into basic blocks and find the type of every evaluation-stack slot,
//  3. emit one wasm function: every (stack depth, type) pair is a wasm local, so the evaluation stack costs nothing once the
//     engine's optimiser has seen it; control flow is `loop { block*N { br_table pc } code0 code1 ... }`, which handles any
//     control-flow graph,
//  4. hand the module's bytes to the host (dna.emit_wasm), which instantiates it and puts the function in the indirect table.

#include "Compat.h"
#include "Sys.h"
#include "JIT.h"
#include "WasmJIT.h"

#if WASM_JIT

#include "MetaData.h"
#include "Types.h"
#include "Type.h"
#include "EvalStack.h"
#include "Heap.h"
#include "HeapEntry.h"
#include "CLIFile.h"
#include "System.Array.h"
#include <stdlib.h>
#include <string.h>

// The host (tools/run_wasm.mjs, or a browser page): compiles and instantiates a module that imports this module's memory, appends its
// exported function "f" to the indirect function table and returns the index; or returns -1 if the module is not valid.
__attribute__((import_module("dna"), import_name("emit_wasm")))
extern int dna_emit_wasm(const unsigned char *bytes, unsigned len);

#define MAX_STACK 64

// What compiled code calls to allocate: C functions, reached through the indirect function table (a function pointer is an index into it).
// The module has their two signatures first: type 0 is (i32) -> i32 and type 1 is (i32, i32) -> i32.
#define SIG_NEWOBJ 0
#define SIG_NEWARR 1
HEAP_PTR WasmJIT_NewObj(tMD_TypeDef *pType) { return Heap_AllocType(pType); }
HEAP_PTR WasmJIT_NewArr(tMD_TypeDef *pArrayType, U32 length) { return length > SystemArray_MaxLength(pArrayType) ? NULL : SystemArray_NewVector(pArrayType, length); }   // (NULL: the length is not valid)

U32 WasmJIT_ChainLen;
const tWasmFrameDesc *WasmJIT_ChainDesc[WJ_MAX_CHAIN];
PTR WasmJIT_ChainImage[WJ_MAX_CHAIN];
U32 WasmJIT_DeoptPush(const tWasmFrameDesc *pDesc, PTR pImage) {         // (returns a value, as the signature that compiled code calls it through does)
	WasmJIT_ChainDesc[WasmJIT_ChainLen] = pDesc; WasmJIT_ChainImage[WasmJIT_ChainLen] = pImage; WasmJIT_ChainLen++;
	return 0;
}

// ---- a growable byte buffer
typedef struct { U8 *p; U32 n, cap; } Buf;
static void BPut(Buf *b, U32 v) {
	if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->p = (U8*)realloc(b->p, b->cap); }
	b->p[b->n++] = (U8)v;
}
static void BMem(Buf *b, const void *m, U32 len) { U32 i; for (i = 0; i < len; i++) BPut(b, ((const U8*)m)[i]); }
static void BU(Buf *b, U32 v) { do { U32 c = v & 0x7f; v >>= 7; BPut(b, v ? c | 0x80 : c); } while (v); }
static void BS(Buf *b, I64 v) {
	for (;;) {
		U32 c = (U32)(v & 0x7f);
		v >>= 7;                                       // (arithmetic shift)
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { BPut(b, c); return; }
		BPut(b, c | 0x80);
	}
}
static void BSection(Buf *out, U32 id, Buf *body) { BPut(out, id); BU(out, body->n); BMem(out, body->p, body->n); }

// ---- value kinds. A reference, a pointer and a native int are 32 bits here (this file is for wasm32 only).
//   T_REF: an object reference (may be null).   T_PTR: a managed pointer (the address of a struct local, a field, an array element).
//   T_VT:  a value type (a struct). It lives in memory: on the compile-time stack it is the ADDRESS of a temporary copy of it (the type is
//          in stTy), so that `this`, ldloca, nested structs and arrays of structs all come down to addresses.
enum { T_I32, T_I64, T_F32, T_F64, T_REF, T_PTR, T_VT, NUM_T };
static const U8 wt[NUM_T] = { 0x7f, 0x7e, 0x7d, 0x7c, 0x7f, 0x7f, 0x7f };
#define IS_ADDR(t) ((t) == T_REF || (t) == T_PTR)
#define IS_INT32(t) ((t) == T_I32 || IS_ADDR(t))
#define MAX_FNS 32
#define MAX_DEPTH 8

// narrowing applied when storing to a local / argument of a small type
enum { N_NONE, N_I8, N_U8, N_I16, N_U16 };

typedef struct { U32 off; U32 op; U32 len; I64 i; double d; U32 target; void *ref; U32 *sw; } Ins;      // sw: the targets of a switch (i of them)     // op: a CIL opcode, normalised (see Decode)

// The control-flow graph of a function's basic blocks, for emitting structured code (wasm loops, blocks and ifs): reverse postorder, dominators,
// which blocks are loop headers and which are merge points (two or more forward edges in). `outer`: a loop header that is in no other loop.
typedef struct {
	U32 n, nOrder;
	U32 *order; int *rpo; int *idom;                  // blocks in reverse postorder; the position of each (-1: not reachable); immediate dominator
	U32 *succStart, *succ;                           // the successors of block b: succ[succStart[b] .. succStart[b+1])
	U8 *isHeader, *isMerge, *outer;
	U32 *kidStart, *kids;                             // the merge nodes each node immediately dominates, in reverse postorder
	U32 anyOuter;
} Analysis;
enum { FR_BLOCK, FR_LOOP, FR_IF };                    // what a wasm label is, while emitting structured code

typedef struct Fn_ Fn;
#define MAX_PREPS 48
typedef struct {
	Fn *fns[MAX_FNS]; U32 nFns; int depth; const char *why; Fn *preps[MAX_PREPS]; U32 nPreps; int prepOnly;
	// the method being compiled, and, made when a virtual call is first met, the interpreter's version of it (see tWasmDeopt)
	tMD_MethodDef *entryM; const U8 *entryCil; U32 entrySize; const tParameter *entryLocals; U32 entryNumLocals, entryLocalsSize, headerMaxStack;
	tJITted *interp; U32 *cilToOp; int filledUsed;
	tWasmDeoptSite *sites; U32 nSites, capSites; U32 maxStackBytes;
} Mod;      // a module: the entry function (0) and what it calls
typedef struct { U32 local; U32 off; } Addr;                                        // a wasm local holding an address, and a constant added to it

struct Fn_ {
	Mod *mod; tMD_MethodDef *m;
	int isEntry, inProgress;
	U32 index;                      // in the module
	const U8 *cil; U32 size;
	Ins *ins; U32 n;
	I32 *insAt;                     // CIL offset -> instruction index, or -1
	U32 *blockOf, *blockStart, nBlocks;
	U8 *entryKnown; U8 (*entryT)[MAX_STACK]; tMD_TypeDef *(*entryTy)[MAX_STACK]; U32 *entryN;
	U32 nArgs, nLocals;
	U8 *argT, *locT, *argN, *locN;  // kind and narrowing of each
	tMD_TypeDef **argTy, **locTy;   // the type of a struct
	U32 *argL, *locL;               // the wasm local of each (for a struct: of the argument, its address; none for a struct local)
	U32 *locOff;                    // the offset of each local in the locals part of the frame
	U32 *locHome;                   // the offset in the scratch area of each struct local
	I32 slot[MAX_STACK][NUM_T];     // wasm local of (depth, kind), or -1
	U8 localKinds[2048]; U32 numWasmLocals, numParams;
	U32 hasLoop;                                    // some backward branch exists: the entry function can be resumed
	U32 pcLocal, sbLocal, tmpA, tmpB, sretLocal, retKind, hasRet; tMD_TypeDef *retTy;
	U8 st[MAX_STACK]; tMD_TypeDef *stTy[MAX_STACK]; U32 sp;
	struct { tMD_TypeDef *t; U32 off, cls; } tmp[MAX_STACK][4]; U8 nTmp[MAX_STACK];
	U32 scratchTop, frameSize, need;                // the scratch area of this function: where it ends, rounded; with its callees
	U32 *patches, nPatches, capPatches;             // where in the body the frame size goes, for the calls
	Fn **callees; U32 nCallees;
	int canThrow, canDeopt, failed;
	tParameter *locParams; tWasmFrameDesc **descs; U32 nDescs, localsBytes;      // (a function that is called) the frames that it can give to the interpreter, and the size of its locals
	// structured control flow (see EmitStructuredFn): loops, blocks and ifs, not the dispatch loop
	int structured, isClone, isDispatcher;
	Fn *master;                                        // for a variant of the entry function: the function that owns the decoded code
	U32 variantRoot, homesTop;
	U32 *loopId, *loopHeaders, nLoopIds;               // (master) the outermost loops that the entry function can resume at: block -> id (1..), id -> block
	U32 *variantFn, nVariantFns;                       // (dispatcher) the function to call for each value of `entry`
	Analysis *an;
	struct { U8 kind; U32 id; } *frames; U32 nFrames, capFrames;
	// scalar replacement of structs (see Ref): where the value or pointer at each stack depth is; the wasm locals that hold a struct value of a
	// type at a depth; and where each argument and local is
	struct Ref_ *stRef;                                  // [MAX_STACK], for the entries that are T_VT or T_PTR
	struct { tMD_TypeDef *t; U32 local[12]; } bank[MAX_STACK][4]; U8 nBank[MAX_STACK];
	struct Ref_ *argRef, *locRef;
	// inlining: the instructions being emitted (the function's own, or an inlined callee's), and what is being inlined
	Ins *curIns;
	struct { int active; U32 base; int hasRet; U32 retKind; tMD_TypeDef *retTy; } inl;
	U32 inlDepth, inlBudget; tMD_MethodDef *inlStack[8];
	U32 entryBase;                  // (entry) where the scratch area starts in the interpreter's frame
	Buf body;
	const char *why;
};

#define FAIL(msg) do { cx->why = (msg); return 0; } while (0)

static U32 NewLocal(Fn *cx, int kind) {
	if (cx->numWasmLocals >= sizeof(cx->localKinds)) { cx->failed = 1; cx->why = "too many locals"; return 0; }
	cx->localKinds[cx->numWasmLocals] = (U8)kind;
	return cx->numParams + cx->numWasmLocals++;
}
static U32 Slot(Fn *cx, U32 depth, int kind) {
	if (cx->slot[depth][kind] < 0) cx->slot[depth][kind] = (I32)NewLocal(cx, kind);
	return (U32)cx->slot[depth][kind];
}

// ---- emit helpers
#define OP(b) BPut(&cx->body, (b))
#define LGET(i) do { OP(0x20); BU(&cx->body, (i)); } while (0)
#define LSET(i) do { OP(0x21); BU(&cx->body, (i)); } while (0)
static void I32Const(Fn *cx, I32 v) { OP(0x41); BS(&cx->body, v); }
static void I64Const(Fn *cx, I64 v) { OP(0x42); BS(&cx->body, v); }
static void F32Const(Fn *cx, float v) { OP(0x43); BMem(&cx->body, &v, 4); }
static void F64Const(Fn *cx, double v) { OP(0x44); BMem(&cx->body, &v, 8); }
static void Mem(Fn *cx, U32 op, U32 offset) { OP(op); BU(&cx->body, 0); BU(&cx->body, offset); }
static void Return(Fn *cx, I32 status) { I32Const(cx, status); OP(0x0f); }      // (the entry function only: its result is the status)
static U32 LoadOp(int kind) { return kind == T_I64 ? 0x29 : kind == T_F32 ? 0x2a : kind == T_F64 ? 0x2b : 0x28; }
static U32 StoreOp(int kind) { return kind == T_I64 ? 0x37 : kind == T_F32 ? 0x38 : kind == T_F64 ? 0x39 : 0x36; }

// A function that returns because of an exception: the entry function returns the status; the others set the module's status global (which
// their callers look at after the call) and return a dummy result.
static void PushZeroRet(Fn *cx) {
	if (!cx->hasRet || cx->retKind == T_VT) return;
	switch (cx->retKind) { case T_I64: I64Const(cx, 0); break; case T_F32: F32Const(cx, 0); break; case T_F64: F64Const(cx, 0); break; default: I32Const(cx, 0); }
}
static void Throw(Fn *cx, I32 status) {
	cx->canThrow = 1;
	I32Const(cx, status);
	if (!cx->isEntry) { OP(0x24); BU(&cx->body, 0); PushZeroRet(cx); }
	OP(0x0f);
}

// ---- the scratch area: memory in the interpreter's frame (zeroed, scanned by the collector, and kept while the thread is not running) where
// struct locals and the temporaries that hold struct values live. A function's callees' areas follow its own.
static U32 AllocScratch(Fn *cx, U32 size) { U32 off = cx->scratchTop; cx->scratchTop += (size + 7) & ~7u; return off; }
static U32 TySize(tMD_TypeDef *t) {
	U32 s = t->stackSize;
	if (t->instanceMemSize > s) s = t->instanceMemSize;
	if (t->arrayElementSize > s) s = t->arrayElementSize;
	return s;
}
// the temporary for a struct value of type t at stack depth d. cls: 0 a value of the stack, 1 the result of a call, 2 a new object
static U32 TempOff(Fn *cx, U32 depth, tMD_TypeDef *t, U32 cls) {
	U32 i;
	for (i = 0; i < cx->nTmp[depth]; i++) if (cx->tmp[depth][i].t == t && cx->tmp[depth][i].cls == cls) return cx->tmp[depth][i].off;
	if (cx->nTmp[depth] >= 4) { cx->failed = 1; cx->why = "too many struct temporaries"; return 0; }
	cx->tmp[depth][cx->nTmp[depth]].t = t; cx->tmp[depth][cx->nTmp[depth]].cls = cls;
	cx->tmp[depth][cx->nTmp[depth]].off = AllocScratch(cx, TySize(t));
	return cx->tmp[depth][cx->nTmp[depth]++].off;
}
static void CopyBytes(Fn *cx, Addr dst, Addr src, U32 size) {
	U32 k = 0;
	if (size > 64) {                     // memory.copy
		LGET(dst.local); I32Const(cx, dst.off); OP(0x6a); LGET(src.local); I32Const(cx, src.off); OP(0x6a); I32Const(cx, size);
		OP(0xfc); BU(&cx->body, 10); OP(0); OP(0);
		return;
	}
	while (size - k >= 8) { LGET(dst.local); LGET(src.local); Mem(cx, 0x29, src.off + k); Mem(cx, 0x37, dst.off + k); k += 8; }
	if (size - k >= 4) { LGET(dst.local); LGET(src.local); Mem(cx, 0x28, src.off + k); Mem(cx, 0x36, dst.off + k); k += 4; }
	if (size - k >= 2) { LGET(dst.local); LGET(src.local); Mem(cx, 0x2f, src.off + k); Mem(cx, 0x3b, dst.off + k); k += 2; }
	if (size - k >= 1) { LGET(dst.local); LGET(src.local); Mem(cx, 0x2d, src.off + k); Mem(cx, 0x3a, dst.off + k); }
}
static void ZeroBytes(Fn *cx, Addr a, U32 size) {
	U32 k = 0;
	if (size > 64) {                     // memory.fill
		LGET(a.local); I32Const(cx, a.off); OP(0x6a); I32Const(cx, 0); I32Const(cx, size); OP(0xfc); BU(&cx->body, 11); OP(0);
		return;
	}
	while (size - k >= 8) { LGET(a.local); I64Const(cx, 0); Mem(cx, 0x37, a.off + k); k += 8; }
	if (size - k >= 4) { LGET(a.local); I32Const(cx, 0); Mem(cx, 0x36, a.off + k); k += 4; }
	if (size - k >= 2) { LGET(a.local); I32Const(cx, 0); Mem(cx, 0x3b, a.off + k); k += 2; }
	if (size - k >= 1) { LGET(a.local); I32Const(cx, 0); Mem(cx, 0x3a, a.off + k); }
}
// A struct value is about to be pushed at depth d: its temporary's address goes in tmpA, to copy into; VTFinish then makes it the stack entry.
static void VTBegin(Fn *cx, U32 depth, tMD_TypeDef *t, U32 cls) {
	U32 off = TempOff(cx, depth, t, cls);
	LGET(cx->sbLocal); I32Const(cx, off); OP(0x6a); LSET(cx->tmpA);
}
static void VTFinish(Fn *cx, U32 depth, tMD_TypeDef *t) {
	LGET(cx->tmpA); LSET(Slot(cx, depth, T_VT));
	cx->st[depth] = T_VT; cx->stTy[depth] = t;
}
// the frame size of the calling function goes where a call passes its callee the scratch area that follows it: a 5-byte constant, patched later
static void FrameSizeHole(Fn *cx) {
	OP(0x41);
	if (cx->nPatches == cx->capPatches) { cx->capPatches = cx->capPatches ? cx->capPatches * 2 : 8; cx->patches = (U32*)realloc(cx->patches, cx->capPatches * sizeof(U32)); }
	cx->patches[cx->nPatches++] = cx->body.n;
	OP(0x80); OP(0x80); OP(0x80); OP(0x80); OP(0x00);
}

// ---- type mapping
static int KindOfType(tMD_TypeDef *t, U8 *narrow) {
	*narrow = N_NONE;
	if (t == NULL) return -1;
	if (t == types[TYPE_SYSTEM_SBYTE]) *narrow = N_I8;
	else if (t == types[TYPE_SYSTEM_BYTE] || t == types[TYPE_SYSTEM_BOOLEAN]) *narrow = N_U8;
	else if (t == types[TYPE_SYSTEM_INT16]) *narrow = N_I16;
	else if (t == types[TYPE_SYSTEM_UINT16] || t == types[TYPE_SYSTEM_CHAR]) *narrow = N_U16;
	switch (t->stackType) {
	case EVALSTACK_INT32: return T_I32;
	case EVALSTACK_INT64: return T_I64;
	case EVALSTACK_F32: return T_F32;
	case EVALSTACK_F64: return T_F64;
	case EVALSTACK_O: return T_REF;
	case EVALSTACK_VALUETYPE: return T_VT;
	default: return -1;
	}
}
static int KindOfTypeSimple(tMD_TypeDef *t) { U8 nar; return KindOfType(t, &nar); }
static U32 SizeOfKind(int k) { return (k == T_I64 || k == T_F64) ? 8 : 4; }

// ================= scalar replacement of small structs =================
// A struct whose fields (nested structs flattened) are all primitives or references, at most MAXLEAF of them, can live in wasm locals: one per
// leaf. Then `new Vec3(x, y, z)` is three local sets, not a trip through memory that the engine cannot see through. A struct value, or a
// pointer to one, is described by a Ref: a place in memory (an address held in a wasm local, plus an offset), or in a bank of locals (plus an
// offset into the struct that the bank holds). A pointer to a struct local or argument that is held in locals is symbolic: it costs no code, and
// ldfld / stfld through it go straight to the leaf. It becomes an address only if a call needs one (see EmitCall).
#define MAXLEAF 12
typedef struct { U32 n; U32 off[MAXLEAF]; U8 kind[MAXLEAF]; } Leaves;
typedef struct Ref_ { U8 mode; U32 addr, off; const Leaves *L; U32 bank[MAXLEAF]; U8 hasFrame; U32 frameHome; } Ref;     // frameHome: where the struct is in the frame
enum { R_MEM = 1, R_SCALAR = 2 };

static int AddLeaves(tMD_TypeDef *t, U32 base, Leaves *L) {
	U32 i;
	for (i = 0; i < t->numFields; i++) {
		tMD_FieldDef *f = t->ppFields[i];
		U8 nar; int k;
		if (FIELD_ISSTATIC(f)) continue;
		MetaData_Fill_TypeDef(f->pType, NULL, NULL);
		k = KindOfType(f->pType, &nar);
		if (k < 0 || k == T_PTR) return 0;
		if (k == T_VT) { if (!AddLeaves(f->pType, base + f->memOffset, L)) return 0; }
		else {
			if (f->memSize != SizeOfKind(k) || L->n >= MAXLEAF) return 0;
			L->off[L->n] = base + f->memOffset; L->kind[L->n] = (U8)k; L->n++;
		}
	}
	return 1;
}
// The leaves of a struct type, or NULL if it cannot be held in locals (a field of another kind, too many, fields that overlap).
static const Leaves *LeavesOf(tMD_TypeDef *t) {
	static struct { tMD_TypeDef *t; Leaves L; int ok; } cache[128];
	static int n = 0, disabled = -1;
	int i; U32 a, b;
	if (disabled < 0) disabled = getenv("DNA_WASM_JIT_NOSROA") != NULL;
	if (disabled || t == NULL || t->stackType != EVALSTACK_VALUETYPE) return NULL;
	for (i = 0; i < n; i++) if (cache[i].t == t) return cache[i].ok ? &cache[i].L : NULL;
	if (n >= 128) return NULL;
	cache[n].t = t; memset(&cache[n].L, 0, sizeof(Leaves));
	MetaData_Fill_TypeDef(t, NULL, NULL);
	cache[n].ok = AddLeaves(t, 0, &cache[n].L) && cache[n].L.n > 0;
	if (cache[n].ok) {
		Leaves *L = &cache[n].L;
		for (a = 0; a < L->n && cache[n].ok; a++) {
			if (L->off[a] + SizeOfKind(L->kind[a]) > t->instanceMemSize) cache[n].ok = 0;
			for (b = a + 1; b < L->n; b++)
				if (L->off[a] < L->off[b] + SizeOfKind(L->kind[b]) && L->off[b] < L->off[a] + SizeOfKind(L->kind[a])) cache[n].ok = 0;
		}
	}
	n++;
	return cache[n - 1].ok ? &cache[n - 1].L : NULL;
}

static Ref RefMem(U32 addrLocal, U32 off) { Ref r; memset(&r, 0, sizeof(r)); r.mode = R_MEM; r.addr = addrLocal; r.off = off; return r; }
static int LeafIndex(const Ref *r, U32 rel, int kind) {
	U32 i, abs = r->off + rel;
	for (i = 0; i < r->L->n; i++) if (r->L->off[i] == abs && r->L->kind[i] == kind) return (int)i;
	return -1;
}
// push the leaf at `rel` (relative to what the Ref refers to) on the wasm stack
static int RefLoad(Fn *cx, const Ref *r, U32 rel, int kind) {
	if (r->mode == R_MEM) { LGET(r->addr); Mem(cx, LoadOp(kind), r->off + rel); return 1; }
	{
		int i = LeafIndex(r, rel, kind);
		if (i < 0) { cx->failed = 1; cx->why = "scalar struct: no such field"; return 0; }
		LGET(r->bank[i]);
	}
	return 1;
}
static int RefStore(Fn *cx, const Ref *r, U32 rel, int kind, U32 valueLocal) {
	if (r->mode == R_MEM) { LGET(r->addr); LGET(valueLocal); Mem(cx, StoreOp(kind), r->off + rel); return 1; }
	{
		int i = LeafIndex(r, rel, kind);
		if (i < 0) { cx->failed = 1; cx->why = "scalar struct: no such field"; return 0; }
		LGET(valueLocal); LSET(r->bank[i]);
	}
	return 1;
}
// copy a struct of type t: bytes if both are in memory (memBytes of them), else leaf by leaf
static int RefCopy(Fn *cx, const Ref *dst, const Ref *src, tMD_TypeDef *t, U32 memBytes) {
	const Leaves *L; U32 i;
	if (dst->mode == R_MEM && src->mode == R_MEM) { Addr d = { dst->addr, dst->off }, sa = { src->addr, src->off }; CopyBytes(cx, d, sa, memBytes); return 1; }
	L = LeavesOf(t);
	if (L == NULL) { cx->failed = 1; cx->why = "struct copy between forms for a type without leaves"; return 0; }
	for (i = 0; i < L->n; i++) {
		if (dst->mode == R_MEM) { LGET(dst->addr); if (!RefLoad(cx, src, L->off[i], L->kind[i])) return 0; Mem(cx, StoreOp(L->kind[i]), dst->off + L->off[i]); }
		else {
			int di = LeafIndex(dst, L->off[i], L->kind[i]);
			if (di < 0) { cx->failed = 1; cx->why = "scalar struct: no such field"; return 0; }
			if (!RefLoad(cx, src, L->off[i], L->kind[i])) return 0;
			LSET(dst->bank[di]);
		}
	}
	return 1;
}
static int RefZero(Fn *cx, const Ref *dst, tMD_TypeDef *t, U32 memBytes) {
	const Leaves *L; U32 i;
	if (dst->mode == R_MEM) { Addr a = { dst->addr, dst->off }; ZeroBytes(cx, a, memBytes); return 1; }
	L = LeavesOf(t);
	if (L == NULL) { cx->failed = 1; cx->why = "zeroing a scalar struct without leaves"; return 0; }
	for (i = 0; i < L->n; i++) {
		int di = LeafIndex(dst, L->off[i], L->kind[i]);
		if (di < 0) { cx->failed = 1; cx->why = "scalar struct: no such field"; return 0; }
		switch (L->kind[i]) { case T_I64: I64Const(cx, 0); break; case T_F32: F32Const(cx, 0); break; case T_F64: F64Const(cx, 0); break; default: I32Const(cx, 0); }
		LSET(dst->bank[di]);
	}
	return 1;
}
// the wasm locals that hold a struct value of type t at stack depth `depth`
static U32 *StackBank(Fn *cx, U32 depth, tMD_TypeDef *t, const Leaves *L) {
	U32 i, j;
	for (i = 0; i < cx->nBank[depth]; i++) if (cx->bank[depth][i].t == t) return cx->bank[depth][i].local;
	if (cx->nBank[depth] >= 4) { cx->failed = 1; cx->why = "too many struct banks"; return cx->bank[depth][0].local; }
	cx->bank[depth][i].t = t;
	for (j = 0; j < L->n; j++) cx->bank[depth][i].local[j] = NewLocal(cx, L->kind[j]);
	cx->nBank[depth]++;
	return cx->bank[depth][i].local;
}
// Push at `depth` a struct value of type t copied from src: in locals if the type has leaves, else in a memory temporary.
static int PushVTFrom(Fn *cx, U32 depth, tMD_TypeDef *t, const Ref *src, U32 memBytes) {
	const Leaves *L = LeavesOf(t);
	Ref dst;
	if (L != NULL) {
		U32 *bank = StackBank(cx, depth, t, L), i;
		memset(&dst, 0, sizeof(dst)); dst.mode = R_SCALAR; dst.L = L;
		for (i = 0; i < L->n; i++) dst.bank[i] = bank[i];
		if (!RefCopy(cx, &dst, src, t, memBytes)) return 0;
	} else {
		VTBegin(cx, depth, t, 0);
		dst = RefMem(cx->tmpA, 0);
		if (!RefCopy(cx, &dst, src, t, memBytes)) return 0;
		LGET(cx->tmpA); LSET(Slot(cx, depth, T_VT));
		dst = RefMem(Slot(cx, depth, T_VT), 0);
	}
	cx->st[depth] = T_VT; cx->stTy[depth] = t; cx->stRef[depth] = dst;
	return 1;
}
// A struct value that is held in locals goes to memory (a temporary): a value that flows into another block must be in one place, and a call
// takes addresses.
static int MaterializeVT(Fn *cx, U32 depth, U32 cls) {
	tMD_TypeDef *t = cx->stTy[depth];
	Ref dst, src = cx->stRef[depth];
	U32 off;
	if (src.mode != R_SCALAR) return 1;
	off = TempOff(cx, depth, t, cls);
	if (cx->failed) return 0;
	LGET(cx->sbLocal); I32Const(cx, off); OP(0x6a); LSET(Slot(cx, depth, T_VT));
	dst = RefMem(Slot(cx, depth, T_VT), 0);
	if (!RefCopy(cx, &dst, &src, t, TySize(t))) return 0;
	cx->stRef[depth] = dst;
	return 1;
}
// the memory form of the stack entries, as seen by a block entered with them
static void RestoreRefs(Fn *cx) {
	U32 i;
	for (i = 0; i < cx->sp; i++) {
		if (cx->st[i] == T_VT) cx->stRef[i] = RefMem(Slot(cx, i, T_VT), 0);
		else if (cx->st[i] == T_PTR) cx->stRef[i] = RefMem(Slot(cx, i, T_PTR), 0);
	}
}


// ---- calls that become a single instruction. The semantics of each are those of the corlib method it replaces (checked by tests/dotnet/WasmJit.cs):
// Abs clears the sign bit; Min and Max give NaN if either is NaN and order -0 before +0; Floor, Ceiling, Truncate, Sqrt are IEEE.
enum { IK_SQRT = 1, IK_ABS, IK_MIN, IK_MAX, IK_FLOOR, IK_CEIL, IK_TRUNC };
#define IK(op, kind, uns) ((I64)((op) | ((kind) << 8) | ((uns) << 16)))

static int ResolveIntrinsic(Fn *cx, U32 token, I64 *out) {
	tMD_MethodDef *c = MetaData_GetMethodDefFromDefRefOrSpec(cx->m->pMetaData, token, cx->m->pParentType->ppClassTypeArgs, cx->m->ppMethodTypeArgs);
	tMD_TypeDef *p;
	const char *name;
	U8 nar;
	int op = 0, kind, uns = 0, nargs;
	if (c->isFilled == 0) MetaData_Fill_TypeDef(MetaData_GetTypeDefFromMethodDef(c), NULL, NULL);
	p = c->pParentType;
	if (p == NULL || p->nameSpace == NULL || strcmp((const char*)p->nameSpace, "System") != 0) return 0;
	if (strcmp((const char*)p->name, "Math") != 0 && strcmp((const char*)p->name, "MathF") != 0) return 0;
	name = (const char*)c->name;
	if (!strcmp(name, "Sqrt")) op = IK_SQRT; else if (!strcmp(name, "Abs")) op = IK_ABS; else if (!strcmp(name, "Min")) op = IK_MIN;
	else if (!strcmp(name, "Max")) op = IK_MAX; else if (!strcmp(name, "Floor")) op = IK_FLOOR; else if (!strcmp(name, "Ceiling")) op = IK_CEIL;
	else if (!strcmp(name, "Truncate")) op = IK_TRUNC; else return 0;
	nargs = (op == IK_MIN || op == IK_MAX) ? 2 : 1;
	if (c->numberOfParameters != nargs || c->pReturnType != c->pParams[0].pTypeDef) return 0;
	if (nargs == 2 && c->pParams[1].pTypeDef != c->pParams[0].pTypeDef) return 0;
	{
		tMD_TypeDef *t = c->pParams[0].pTypeDef;
		if (t == types[TYPE_SYSTEM_SINGLE]) kind = T_F32;
		else if (t == types[TYPE_SYSTEM_DOUBLE]) kind = T_F64;
		else if (t == types[TYPE_SYSTEM_INT32]) kind = T_I32;
		else if (t == types[TYPE_SYSTEM_INT64]) kind = T_I64;
		else if (t == types[TYPE_SYSTEM_UINT32]) { kind = T_I32; uns = 1; }
		else if (t == types[TYPE_SYSTEM_UINT64]) { kind = T_I64; uns = 1; }
		else return 0;
	}
	(void)nar;
	// (only the float forms of the unary ones; of the integer ones only Min and Max: Abs(int) throws on MinValue)
	if ((kind == T_I32 || kind == T_I64) && op != IK_MIN && op != IK_MAX) return 0;
	*out = IK(op, kind, uns);
	return 1;
}

// ---- 1. decode
static int Decode(Fn *cx) {
	U32 o = 0, cap = cx->size + 1;
	cx->ins = (Ins*)calloc(cap, sizeof(Ins));
	cx->insAt = (I32*)malloc((cx->size + 1) * sizeof(I32));
	memset(cx->insAt, 0xff, (cx->size + 1) * sizeof(I32));
	cx->n = 0;
	while (o < cx->size) {
		Ins *in = &cx->ins[cx->n];
		U32 op = cx->cil[o], len = 1;
		const U8 *p = cx->cil + o + 1;
		U32 remain = cx->size - o - 1;
		#define NEED(k) if (remain < (k)) FAIL("truncated")
		in->off = o;
		if (op == 0xfe) { NEED(1); op = 0xfe00 | cx->cil[o + 1]; len = 2; p++; remain--; }
		in->i = 0; in->d = 0; in->target = 0; in->ref = NULL; in->sw = NULL;
		if (op == 0x00) { /* nop */ }
		else if (op >= 0x02 && op <= 0x05) { in->op = 0x0e; in->i = op - 0x02; goto done; }
		else if (op >= 0x06 && op <= 0x09) { in->op = 0x11; in->i = op - 0x06; goto done; }
		else if (op >= 0x0a && op <= 0x0d) { in->op = 0x13; in->i = op - 0x0a; goto done; }
		else if (op == 0x0e || op == 0x11 || op == 0x13 || op == 0x10) { NEED(1); in->op = op == 0x10 ? 0x10 : op; in->i = p[0]; len = 2; goto done; }
		else if (op >= 0x15 && op <= 0x1e) { in->op = 0x20; in->i = (I32)op - 0x16; goto done; }
		else if (op == 0x1f) { NEED(1); in->op = 0x20; in->i = (signed char)p[0]; len = 2; goto done; }
		else if (op == 0x20) { NEED(4); in->op = 0x20; in->i = *(const I32*)p; len = 5; goto done; }
		else if (op == 0x21) { NEED(8); in->op = 0x21; memcpy(&in->i, p, 8); len = 9; goto done; }
		else if (op == 0x22) { NEED(4); float f; memcpy(&f, p, 4); in->op = 0x22; in->d = f; len = 5; goto done; }
		else if (op == 0x23) { NEED(8); memcpy(&in->d, p, 8); in->op = 0x23; len = 9; goto done; }
		else if (op == 0x25 || op == 0x26 || op == 0x2a || op == 0x14) { in->op = op; goto done; }       // dup pop ret ldnull
		else if (op >= 0x2b && op <= 0x37) { NEED(1); in->op = op + 0x0d; in->target = o + 2 + (signed char)p[0]; len = 2; goto done; }     // short branch -> long
		else if (op >= 0x38 && op <= 0x44) { NEED(4); in->op = op; in->target = o + 5 + *(const I32*)p; len = 5; goto done; }
		else if ((op >= 0x58 && op <= 0x5e) || (op >= 0x5f && op <= 0x66)) { in->op = op; goto done; }       // add sub mul div div.un rem rem.un, and ... not
		else if (op == 0x45) {                                                                              // switch
			U32 cnt, j;
			NEED(4);
			cnt = *(const U32*)p;
			if (cnt > 8192) FAIL("switch table too large");
			NEED(4 + 4 * cnt);
			in->sw = (U32*)malloc((cnt + 1) * sizeof(U32));
			for (j = 0; j < cnt; j++) in->sw[j] = (U32)(o + 5 + 4 * cnt + *(const I32*)(p + 4 + 4 * j));     // (CIL offsets, until the second pass)
			in->op = 0x45; in->i = cnt; len = 5 + 4 * cnt; goto done;
		}
		else if (op == 0x0f || op == 0x12) { NEED(1); in->op = op; in->i = p[0]; len = 2; goto done; }          // ldarga.s ldloca.s
		else if (op == 0xfe09 || op == 0xfe0b || op == 0xfe0c || op == 0xfe0e) {                            // ldarg starg ldloc stloc, long forms (a 16-bit index)
			NEED(2); in->op = op == 0xfe09 ? 0x0e : op == 0xfe0b ? 0x10 : op == 0xfe0c ? 0x11 : 0x13; in->i = *(const U16*)p; len = 4; goto done;
		}
		else if (op == 0xfe0a || op == 0xfe0d) { NEED(2); in->op = op == 0xfe0a ? 0x0f : 0x12; in->i = *(const U16*)p; len = 4; goto done; }
		else if (op == 0x7b || op == 0x7c || op == 0x7d || op == 0x7e || op == 0x80) {                      // ldfld ldflda stfld ldsfld stsfld
			tMD_FieldDef *f;
			NEED(4);
			f = MetaData_GetFieldDefFromDefOrRef(cx->m->pMetaData, *(const U32*)p, cx->m->pParentType->ppClassTypeArgs, cx->m->ppMethodTypeArgs);
			MetaData_Fill_TypeDef(MetaData_GetTypeDefFromFieldDef(f), NULL, NULL);
			if (f->pType != NULL) MetaData_Fill_TypeDef(f->pType, NULL, NULL);
			in->op = op; in->ref = f; len = 5; goto done;
		}
		else if (op == 0x71 || op == 0x81 || op == 0xfe15 || op == 0xa3 || op == 0xa4 || op == 0x8f) {      // ldobj stobj initobj ldelem stelem ldelema <type>
			tMD_TypeDef *t;
			NEED(4);
			t = MetaData_GetTypeDefFromDefRefOrSpec(cx->m->pMetaData, *(const U32*)p, cx->m->pParentType->ppClassTypeArgs, cx->m->ppMethodTypeArgs);
			MetaData_Fill_TypeDef(t, NULL, NULL);
			in->op = op; in->ref = t; in->i = t->arrayElementSize; len = (op == 0xfe15) ? 6 : 5; goto done;
		}
		else if (op == 0x28 || op == 0x6f || op == 0x73) {                                                  // call callvirt newobj
			tMD_MethodDef *c;
			NEED(4);
			if (op == 0x28 && ResolveIntrinsic(cx, *(const U32*)p, &in->i)) { in->op = 0x28; in->ref = NULL; len = 5; goto done; }
			c = MetaData_GetMethodDefFromDefRefOrSpec(cx->m->pMetaData, *(const U32*)p, cx->m->pParentType->ppClassTypeArgs, cx->m->ppMethodTypeArgs);
			if (c->isFilled == 0) MetaData_Fill_TypeDef(MetaData_GetTypeDefFromMethodDef(c), NULL, NULL);
			in->op = op; in->ref = c; in->i = 0; len = 5; goto done;
		}
		else if (op == 0x8d) {                                                                              // newarr <element type>
			tMD_TypeDef *t;
			NEED(4);
			t = MetaData_GetTypeDefFromDefRefOrSpec(cx->m->pMetaData, *(const U32*)p, cx->m->pParentType->ppClassTypeArgs, cx->m->ppMethodTypeArgs);
			MetaData_Fill_TypeDef(t, NULL, NULL);
			in->op = 0x8d; in->ref = Type_GetArrayTypeDef(t, cx->m->pParentType->ppClassTypeArgs, cx->m->ppMethodTypeArgs); len = 5; goto done;
		}
		else if (op == 0x4a || op == 0x4b || op == 0x4e || op == 0x4f || op == 0x54 || op == 0x56 || op == 0x57) { in->op = op; goto done; }   // ldind / stind of i4 r4 r8
		else if (op == 0x67 || op == 0x68 || op == 0x69 || op == 0x6a || op == 0x6b || op == 0x6c || op == 0x6d || op == 0x6e) { in->op = op; goto done; }
		else if (op == 0xd1 || op == 0xd2 || op == 0xd3 || op == 0xe0) { in->op = op; goto done; }
		else if (op == 0x8e) { in->op = op; goto done; }                                                  // ldlen
		else if (op == 0x94 || op == 0x95 || op == 0x98 || op == 0x99 || op == 0x9a || op == 0xa2 || op == 0x9e || op == 0xa0 || op == 0xa1) { in->op = op; goto done; }
		else if (op >= 0xfe01 && op <= 0xfe05) { in->op = op; goto done; }
		else {
			static char why[48];
			snprintf(why, sizeof(why), "unsupported opcode 0x%x", op);
			FAIL(why);
		}
		in->op = 0x00;
	done:
		cx->insAt[o] = (I32)cx->n;
		o += len;
		cx->n++;
		#undef NEED
	}
	if (cx->n == 0) FAIL("empty");
	{
		U32 k;
		for (k = 0; k < cx->n; k++) {
			Ins *in = &cx->ins[k];
			if (in->op >= 0x38 && in->op <= 0x44) {
				if (in->target > cx->size || cx->insAt[in->target] < 0) FAIL("bad branch target");
				in->target = (U32)cx->insAt[in->target];        // now an instruction index
			}
			if (in->op == 0x45) {
				U32 j;
				for (j = 0; j < (U32)in->i; j++) {
					if (in->sw[j] > cx->size || cx->insAt[in->sw[j]] < 0) FAIL("bad switch target");
					in->sw[j] = (U32)cx->insAt[in->sw[j]];
				}
			}
		}
	}
	return 1;
}

static int IsBranch(U32 op) { return op >= 0x38 && op <= 0x44; }

// ---- 2. basic blocks
static int FindBlocks(Fn *cx) {
	U32 k, b;
	U8 *leader = (U8*)calloc(cx->n + 1, 1);
	leader[0] = 1;
	for (k = 0; k < cx->n; k++) {
		Ins *in = &cx->ins[k];
		if (IsBranch(in->op)) { leader[in->target] = 1; leader[k + 1] = 1; }
		if (in->op == 0x45) { U32 j; for (j = 0; j < (U32)in->i; j++) leader[in->sw[j]] = 1; leader[k + 1] = 1; }
		if (in->op == 0x2a) leader[k + 1] = 1;
	}
	cx->blockOf = (U32*)malloc(cx->n * sizeof(U32));
	cx->blockStart = (U32*)malloc((cx->n + 2) * sizeof(U32));
	cx->nBlocks = 0;
	for (k = 0; k < cx->n; k++) {
		if (leader[k]) cx->blockStart[cx->nBlocks++] = k;
		cx->blockOf[k] = cx->nBlocks - 1;
	}
	cx->blockStart[cx->nBlocks] = cx->n;
	free(leader);
	cx->entryKnown = (U8*)calloc(cx->nBlocks, 1);
	cx->entryT = (U8 (*)[MAX_STACK])calloc(cx->nBlocks, MAX_STACK);
	cx->entryTy = (tMD_TypeDef *(*)[MAX_STACK])calloc(cx->nBlocks, MAX_STACK * sizeof(tMD_TypeDef*));
	cx->entryN = (U32*)calloc(cx->nBlocks, sizeof(U32));
	return 1;
}

// the stack at the end of a block flows into block `to`: it must be the stack that block was entered with before
static int FlowTo(Fn *cx, U32 to) {
	U32 i;
	for (i = 0; i < cx->sp; i++) {      // (what flows into a block must be in one form: structs in locals go to memory; a pointer to one cannot)
		if (cx->st[i] == T_VT && cx->stRef[i].mode == R_SCALAR) { if (!MaterializeVT(cx, i, 0)) return 0; }
		else if (cx->st[i] == T_PTR && cx->stRef[i].mode == R_SCALAR) FAIL("a pointer to a struct held in locals flows into another block");
	}
	if (!cx->entryKnown[to]) {
		cx->entryKnown[to] = 1; cx->entryN[to] = cx->sp;
		for (i = 0; i < cx->sp; i++) { cx->entryT[to][i] = cx->st[i]; cx->entryTy[to][i] = cx->stTy[i]; }
		return 1;
	}
	if (cx->entryN[to] != cx->sp) FAIL("stack depth differs at a join");
	for (i = 0; i < cx->sp; i++) if (cx->entryT[to][i] != cx->st[i] || cx->entryTy[to][i] != cx->stTy[i]) FAIL("stack types differ at a join");
	return 1;
}

// ---- comparisons. which: 0 eq, 1 ge, 2 gt, 3 le, 4 lt, 5 ne.un, 6 ge.un, 7 gt.un, 8 le.un, 9 lt.un. Leaves an i32 on the wasm stack.
static int EmitCompare(Fn *cx, int which, int ta, int tb) {
	int fl = (ta == T_F32 || ta == T_F64);
	if (fl ? ta != tb : !((IS_INT32(ta) && IS_INT32(tb)) || (ta == T_I64 && tb == T_I64))) FAIL("compare of mismatched types");
	if (fl) {
		U32 base = ta == T_F32 ? 0x5b : 0x61;                    // eq ne lt gt le ge
		// (un-ordered forms: the negation of the opposite ordered comparison, so NaN makes them true)
		//   ge.un = !(a<b)   gt.un = !(a<=b)   le.un = !(a>b)   lt.un = !(a>=b)
		static const U8 opp[10] = { 0, 0, 0, 0, 0, 0, 2, 4, 3, 5 };      // lt, le, gt, ge as offsets from base for 6..9
		switch (which) {
		case 0: OP(base + 0); break;
		case 1: OP(base + 5); break;
		case 2: OP(base + 3); break;
		case 3: OP(base + 4); break;
		case 4: OP(base + 2); break;
		case 5: OP(base + 1); break;
		default: OP(base + opp[which]); OP(0x45); break;       // i32.eqz
		}
	} else {
		U32 base = ta == T_I64 ? 0x51 : 0x46;                     // eq ne lt_s lt_u gt_s gt_u le_s le_u ge_s ge_u
		static const U8 off[10] = { 0, 8, 4, 6, 2, 1, 9, 5, 7, 3 };
		OP(base + off[which]);
	}
	return 1;
}

// float -> integer the way x86 does it (an out-of-range or NaN value gives the "integer indefinite", the minimum), because wasm's
// plain trunc traps there and the saturating one differs for large positive values
static void EmitFloatToInt(Fn *cx, int from, int to, U32 src) {
	int f32 = (from == T_F32), i64 = (to == T_I64);
	LGET(src);
	OP(0xfc); BU(&cx->body, i64 ? (f32 ? 4 : 6) : (f32 ? 0 : 2));        // trunc_sat
	if (i64) I64Const(cx, (I64)0x8000000000000000ULL); else I32Const(cx, (I32)0x80000000);
	{
		double lo = i64 ? -9223372036854775808.0 : -2147483648.0, hi = -lo;
		LGET(src); if (f32) F32Const(cx, (float)lo); else F64Const(cx, lo); OP(f32 ? 0x60 : 0x66);     // >= lo
		LGET(src); if (f32) F32Const(cx, (float)hi); else F64Const(cx, hi); OP(f32 ? 0x5d : 0x63);     // <  hi
		OP(0x71);                                                                                       // i32.and
	}
	OP(0x1b);                                                                                           // select
}

static void EmitNarrow(Fn *cx, int n) {
	switch (n) {
	case N_I8: OP(0xc0); break;
	case N_I16: OP(0xc1); break;
	case N_U8: I32Const(cx, 0xff); OP(0x71); break;
	case N_U16: I32Const(cx, 0xffff); OP(0x71); break;
	}
}

// ---- 3. emit
// the stores of an argument or local into the frame, and the loads of them: how a function that gives up the processor in a loop keeps
// its state (and the same stores make references visible to the garbage collector while the thread is not running)
// the stores of an argument or local into the frame, and the loads of them: how a function that gives up the processor in a loop keeps
// its state (and the same stores make references visible to the garbage collector while the thread is not running). A struct argument is an
// address into the frame (it is used where it is); a struct local is in the scratch area: neither has state in a wasm local.
static void FrameAll(Fn *cx, int store, int args, int locals) {
	U32 i;
	if (args) for (i = 0; i < cx->nArgs; i++) {
		U32 off = cx->m->pParams[i].offset; int k = cx->argT[i];
		if (k == T_VT) { if (!store) { LGET(0); I32Const(cx, off); OP(0x6a); LSET(cx->argL[i]); } }
		else if (store) { LGET(0); LGET(cx->argL[i]); Mem(cx, StoreOp(k), off); }
		else { LGET(0); Mem(cx, LoadOp(k), off); LSET(cx->argL[i]); }
	}
	if (locals) for (i = 0; i < cx->nLocals; i++) {
		U32 off = cx->m->parameterStackSize + cx->locOff[i]; int k = cx->locT[i];
		if (k == T_VT) {
			if (cx->locRef[i].mode == R_SCALAR) {
				Ref home = RefMem(0, cx->m->parameterStackSize + cx->locOff[i]);                 // (its slot in the frame, which is where the interpreter has it too)
				if (store) RefCopy(cx, &home, &cx->locRef[i], cx->locTy[i], cx->locTy[i]->stackSize);
				else RefCopy(cx, &cx->locRef[i], &home, cx->locTy[i], cx->locTy[i]->stackSize);
			}
			continue;
		}
		if (store) { LGET(0); LGET(cx->locL[i]); Mem(cx, StoreOp(k), off); }
		else { LGET(0); Mem(cx, LoadOp(k), off); LSET(cx->locL[i]); }
	}
}
// at a backward branch to block `tgt`: if (--budget == 0) { save the state; return WJ_RESTART + tgt + 1 }
static void EmitYieldCheck(Fn *cx, U32 entryCode) {
	LGET(3); I32Const(cx, 1); OP(0x6b); OP(0x22); BU(&cx->body, 3); OP(0x45);       // local.tee budget; eqz
	OP(0x04); OP(0x40);
	FrameAll(cx, 1, 1, 1);
	Return(cx, WJ_RESTART + (I32)entryCode);
	OP(0x0b);
}

static Fn *GetCallee(Fn *cx, tMD_MethodDef *c, int isVirt);
static int EmitCall(Fn *cx, Ins *in);

// Has the type's static constructor run, so that compiled code may touch its statics without the check the interpreter makes first
// (JIT_LOADSTATICFIELD_CHECKTYPEINIT_*)? Only where that is certain: a type with no constructor, or one that has run. A generic definition or
// instantiation is never taken to be ready: which type an instantiation's field or method belongs to is not something to rely on here
// (EqualityComparer<T>.Default is exactly that, and reading it before its constructor had run gave null).
static int TypeReady(tMD_TypeDef *t) {
	MetaData_Fill_TypeDef(t, NULL, NULL);
	if (t->isGenericDefinition || t->ppClassTypeArgs != NULL) return 0;
	return t->pStaticConstructor == NULL || t->isTypeInitialised;
}

// the kind, and for a struct the type, of a field; its size in memory must be what the interpreter would read and write
static int FieldInfo(Fn *cx, tMD_FieldDef *f, int *kind, tMD_TypeDef **ty) {
	U8 nar;
	int k = KindOfType(f->pType, &nar);
	if (k < 0 || k == T_PTR) FAIL("field of an unsupported type");
	if (k == T_VT ? f->memSize == 0 : f->memSize != SizeOfKind(k)) FAIL("field with an unusual size");
	*kind = k; *ty = f->pType;
	return 1;
}

#define POPN(k) do { if (cx->sp < (k)) FAIL("stack underflow"); } while (0)

static int EmitIns(Fn *cx, U32 k, U32 blockIdx, int *terminated) {
	Ins *in = &cx->curIns[k];
	U32 sp = cx->sp;
	U32 loopDepth = cx->nBlocks - 1 - blockIdx;           // `br` this far to reach the dispatch loop (from code at block level)
	int ta, tb, tr;
	U32 s1, s2;
	*terminated = 0;
	switch (in->op) {
	case 0x00: break;
	case 0x0e: {      // ldarg
		U32 i = (U32)in->i;
		if (i >= cx->nArgs) FAIL("bad arg");
		if (sp >= MAX_STACK) FAIL("deep stack");
		if (cx->argT[i] == T_VT) {
			if (!PushVTFrom(cx, sp, cx->argTy[i], &cx->argRef[i], cx->argTy[i]->stackSize)) return 0;
		} else if (cx->argT[i] == T_PTR) {                         // 'this' of a struct: where it is
			if (cx->argRef[i].mode == R_SCALAR) cx->stRef[sp] = cx->argRef[i];
			else { LGET(cx->argRef[i].addr); LSET(Slot(cx, sp, T_PTR)); cx->stRef[sp] = RefMem(Slot(cx, sp, T_PTR), 0); }
			cx->st[sp] = T_PTR; cx->stTy[sp] = NULL;
		} else {
			LGET(cx->argL[i]); LSET(Slot(cx, sp, cx->argT[i]));
			cx->st[sp] = cx->argT[i]; cx->stTy[sp] = NULL;
		}
		cx->sp++;
		break; }
	case 0x11: {      // ldloc
		U32 i = (U32)in->i;
		if (i >= cx->nLocals) FAIL("bad local");
		if (sp >= MAX_STACK) FAIL("deep stack");
		if (cx->locT[i] == T_VT) {
			if (!PushVTFrom(cx, sp, cx->locTy[i], &cx->locRef[i], cx->locTy[i]->stackSize)) return 0;
		} else {
			LGET(cx->locL[i]); LSET(Slot(cx, sp, cx->locT[i]));
			cx->st[sp] = cx->locT[i]; cx->stTy[sp] = NULL;
		}
		cx->sp++;
		break; }
	case 0x10:        // starg
	case 0x13: {      // stloc
		int isArg = in->op == 0x10;
		U32 i = (U32)in->i;
		U8 kind, nar; U32 loc;
		POPN(1);
		if (i >= (isArg ? cx->nArgs : cx->nLocals)) FAIL("bad local");
		kind = isArg ? cx->argT[i] : cx->locT[i]; nar = isArg ? cx->argN[i] : cx->locN[i];
		if (kind == T_VT) {
			tMD_TypeDef *ty = isArg ? cx->argTy[i] : cx->locTy[i];
			if (cx->st[sp - 1] != T_VT || cx->stTy[sp - 1] != ty) FAIL("store of a mismatched struct");
			if (!RefCopy(cx, isArg ? &cx->argRef[i] : &cx->locRef[i], &cx->stRef[sp - 1], ty, ty->stackSize)) return 0;
			cx->sp--;
			break;
		}
		loc = isArg ? cx->argL[i] : cx->locL[i];
		if (cx->st[sp - 1] != kind && !(IS_INT32(kind) && IS_INT32(cx->st[sp - 1]) && kind != T_REF && kind != T_PTR)) FAIL("store of mismatched type");
		LGET(Slot(cx, sp - 1, cx->st[sp - 1]));
		EmitNarrow(cx, nar);
		LSET(loc);
		cx->sp--;
		break; }
	case 0x0f: case 0x12: {   // ldarga ldloca: a pointer to a struct argument or local (symbolic if it is held in locals: no code)
		int isArg = in->op == 0x0f;
		U32 i = (U32)in->i;
		const Ref *r;
		if (i >= (isArg ? cx->nArgs : cx->nLocals)) FAIL("bad local");
		if ((isArg ? cx->argT[i] : cx->locT[i]) != T_VT) FAIL("address of a local that is not a struct");
		if (sp >= MAX_STACK) FAIL("deep stack");
		r = isArg ? &cx->argRef[i] : &cx->locRef[i];
		if (r->mode == R_SCALAR) cx->stRef[sp] = *r;
		else { LGET(r->addr); if (r->off) { I32Const(cx, r->off); OP(0x6a); } LSET(Slot(cx, sp, T_PTR)); cx->stRef[sp] = RefMem(Slot(cx, sp, T_PTR), 0); }
		cx->st[sp] = T_PTR; cx->stTy[sp] = NULL; cx->sp++;
		break; }
	case 0x20: if (sp >= MAX_STACK) FAIL("deep stack"); I32Const(cx, (I32)in->i); LSET(Slot(cx, sp, T_I32)); cx->st[cx->sp++] = T_I32; break;
	case 0x21: if (sp >= MAX_STACK) FAIL("deep stack"); I64Const(cx, in->i); LSET(Slot(cx, sp, T_I64)); cx->st[cx->sp++] = T_I64; break;
	case 0x22: if (sp >= MAX_STACK) FAIL("deep stack"); F32Const(cx, (float)in->d); LSET(Slot(cx, sp, T_F32)); cx->st[cx->sp++] = T_F32; break;
	case 0x23: if (sp >= MAX_STACK) FAIL("deep stack"); F64Const(cx, in->d); LSET(Slot(cx, sp, T_F64)); cx->st[cx->sp++] = T_F64; break;
	case 0x45: {      // switch: an int32 selects a target; an index that is out of range (or negative: it is compared as unsigned) falls through
		U32 cnt = (U32)in->i, j, nextBlock = blockIdx + 1;
		POPN(1);
		if (cx->st[sp - 1] != T_I32) FAIL("switch on something that is not an int32");
		cx->sp--;
		if (cx->structured) { LGET(Slot(cx, sp - 1, T_I32)); break; }       // (the index is on the wasm stack; the structure emits the br_table)
		// the dispatch loop: pc is set from the index, in a ladder of blocks that br_table jumps into
		if (nextBlock >= cx->nBlocks) FAIL("switch at the end of the method");
		for (j = 0; j < cnt; j++) if (!FlowTo(cx, cx->blockOf[in->sw[j]])) return 0;
		if (!FlowTo(cx, nextBlock)) return 0;
		for (j = 0; j <= cnt; j++) { OP(0x02); OP(0x40); }
		LGET(Slot(cx, sp - 1, T_I32));
		OP(0x0e); BU(&cx->body, cnt); for (j = 0; j < cnt; j++) BU(&cx->body, j); BU(&cx->body, cnt);
		for (j = 0; j <= cnt; j++) {
			U32 tgt = j < cnt ? cx->blockOf[in->sw[j]] : nextBlock;
			OP(0x0b);                                                           // (after the end of block j, cnt - j of the blocks are still open)
			if (cx->isEntry && tgt <= blockIdx && cx->sp == 0) EmitYieldCheck(cx, tgt + 1);
			I32Const(cx, (I32)tgt); LSET(cx->pcLocal);
			OP(0x0c); BU(&cx->body, loopDepth + (cnt - j));
		}
		*terminated = 1;
		break; }
	case 0x14: if (sp >= MAX_STACK) FAIL("deep stack"); I32Const(cx, 0); LSET(Slot(cx, sp, T_REF)); cx->st[sp] = T_REF; cx->stTy[sp] = NULL; cx->sp++; break;     // ldnull
	case 0x25:        // dup
		POPN(1); if (sp >= MAX_STACK) FAIL("deep stack");
		if (cx->st[sp - 1] == T_VT) {
			if (!PushVTFrom(cx, sp, cx->stTy[sp - 1], &cx->stRef[sp - 1], cx->stTy[sp - 1]->stackSize)) return 0;
		} else if (cx->st[sp - 1] == T_PTR && cx->stRef[sp - 1].mode == R_SCALAR) {
			cx->stRef[sp] = cx->stRef[sp - 1]; cx->st[sp] = T_PTR; cx->stTy[sp] = NULL;
		} else {
			LGET(Slot(cx, sp - 1, cx->st[sp - 1])); LSET(Slot(cx, sp, cx->st[sp - 1]));
			cx->st[sp] = cx->st[sp - 1]; cx->stTy[sp] = NULL;
			if (cx->st[sp] == T_PTR) cx->stRef[sp] = RefMem(Slot(cx, sp, T_PTR), 0);
		}
		cx->sp++;
		break;
	case 0x26: POPN(1); cx->sp--; break;           // pop
	case 0x2a:        // ret
		if (cx->inl.active) {                       // (of an inlined callee: its value stays on our stack)
			if (cx->inl.hasRet) {
				if (cx->sp != cx->inl.base + 1) FAIL("inlined ret with the wrong stack");
				if (cx->st[cx->inl.base] != cx->inl.retKind || (cx->inl.retKind == T_VT && cx->stTy[cx->inl.base] != cx->inl.retTy)) FAIL("inlined return type mismatch");
			} else if (cx->sp != cx->inl.base) FAIL("inlined ret with the wrong stack");
			*terminated = 1;
			break;
		}
		if (cx->hasRet) {
			if (cx->sp != 1) FAIL("ret with the wrong stack");
			if (cx->st[0] != cx->retKind || (cx->retKind == T_VT && cx->stTy[0] != cx->retTy)) FAIL("return type mismatch");
			if (cx->retKind == T_VT) {
				Ref d = RefMem(cx->isEntry ? 1 : cx->sretLocal, 0);                                  // (entry: where the evaluation stack ends)
				if (!RefCopy(cx, &d, &cx->stRef[0], cx->retTy, cx->retTy->stackSize)) return 0;
			} else if (cx->isEntry) {
				LGET(1); LGET(Slot(cx, 0, cx->st[0])); Mem(cx, StoreOp(cx->st[0]), 0);
			} else {
				LGET(Slot(cx, 0, cx->st[0]));
			}
		} else if (cx->sp != 0) FAIL("ret with the wrong stack");
		if (cx->isEntry) Return(cx, 0); else OP(0x0f);
		*terminated = 1;
		break;

	case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e:     // add sub mul div div.un rem rem.un
	case 0x5f: case 0x60: case 0x61: {                 // and or xor
		U32 base;
		POPN(2); ta = cx->st[sp - 2]; tb = cx->st[sp - 1];
		if (ta != tb && !(IS_INT32(ta) && IS_INT32(tb))) FAIL("arithmetic on mismatched types");
		if (ta == T_REF || ta == T_PTR || ta == T_VT) FAIL("arithmetic on a reference");
		if (in->op >= 0x5c && in->op <= 0x5e && (ta == T_F32 || ta == T_F64)) FAIL("unsigned or remainder of a float");
		if (in->op >= 0x5b && in->op <= 0x5e && (ta == T_I32 || ta == T_I64)) {
			// integer division: the exceptions are the interpreter's: /0 is DivideByZero, MinValue / -1 (and % -1) is Overflow (signed only)
			int i64 = (ta == T_I64), isSigned = (in->op == 0x5b || in->op == 0x5d);
			s2 = Slot(cx, sp - 1, tb);
			LGET(s2); OP(i64 ? 0x50 : 0x45); OP(0x04); OP(0x40); Throw(cx, WJ_DIVZERO); OP(0x0b);
			if (isSigned) {
				s1 = Slot(cx, sp - 2, ta);
				LGET(s2); if (i64) I64Const(cx, -1); else I32Const(cx, -1); OP(i64 ? 0x51 : 0x46);
				LGET(s1); if (i64) I64Const(cx, (I64)0x8000000000000000ULL); else I32Const(cx, (I32)0x80000000); OP(i64 ? 0x51 : 0x46);
				OP(0x71); OP(0x04); OP(0x40); Throw(cx, WJ_OVERFLOW); OP(0x0b);
			}
			s1 = Slot(cx, sp - 2, ta);
			LGET(s1); LGET(s2);
			OP((i64 ? 0x7f : 0x6d) + (in->op - 0x5b));        // div_s div_u rem_s rem_u in wasm's order, which is CIL's
			LSET(s1);
			cx->sp--;
			break;
		}
		if (in->op >= 0x5f && (ta == T_F32 || ta == T_F64)) FAIL("bitwise op on a float");
		s1 = Slot(cx, sp - 2, ta); s2 = Slot(cx, sp - 1, tb);
		LGET(s1); LGET(s2);
		switch (in->op) {
		case 0x58: base = ta == T_I32 ? 0x6a : ta == T_I64 ? 0x7c : ta == T_F32 ? 0x92 : 0xa0; break;
		case 0x59: base = ta == T_I32 ? 0x6b : ta == T_I64 ? 0x7d : ta == T_F32 ? 0x93 : 0xa1; break;
		case 0x5a: base = ta == T_I32 ? 0x6c : ta == T_I64 ? 0x7e : ta == T_F32 ? 0x94 : 0xa2; break;
		case 0x5b: base = ta == T_F32 ? 0x95 : 0xa3; break;
		case 0x5f: base = ta == T_I32 ? 0x71 : 0x83; break;
		case 0x60: base = ta == T_I32 ? 0x72 : 0x84; break;
		default: base = ta == T_I32 ? 0x73 : 0x85; break;
		}
		OP(base); LSET(s1);
		cx->sp--;
		break; }
	case 0x62: case 0x63: case 0x64: {                 // shl shr shr.un: the shift count is an int32
		POPN(2); ta = cx->st[sp - 2]; tb = cx->st[sp - 1];
		if (!(ta == T_I32 || ta == T_I64) || tb != T_I32) FAIL("bad shift operands");
		s1 = Slot(cx, sp - 2, ta); s2 = Slot(cx, sp - 1, tb);
		LGET(s1); LGET(s2);
		if (ta == T_I64) OP(0xad);                                        // i64.extend_i32_u
		OP((ta == T_I32 ? 0x74 : 0x86) + (in->op - 0x62));      // shl, shr_s, shr_u are consecutive in wasm too
		LSET(s1);
		cx->sp--;
		break; }
	case 0x65:        // neg
		POPN(1); ta = cx->st[sp - 1];
		s1 = Slot(cx, sp - 1, ta);
		if (ta == T_I32) { I32Const(cx, 0); LGET(s1); OP(0x6b); }
		else if (ta == T_I64) { I64Const(cx, 0); LGET(s1); OP(0x7d); }
		else if (ta == T_F32) { LGET(s1); OP(0x8c); }
		else if (ta == T_F64) { LGET(s1); OP(0x9a); }
		else FAIL("neg of a reference");
		LSET(s1);
		break;
	case 0x66:        // not
		POPN(1); ta = cx->st[sp - 1]; s1 = Slot(cx, sp - 1, ta);
		if (ta == T_I32) { LGET(s1); I32Const(cx, -1); OP(0x73); }
		else if (ta == T_I64) { LGET(s1); I64Const(cx, -1); OP(0x85); }
		else FAIL("not of a non-integer");
		LSET(s1);
		break;

	case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e:
	case 0xd1: case 0xd2: case 0xd3: case 0xe0: {      // conv.*
		U32 cop = in->op;
		POPN(1); ta = cx->st[sp - 1]; s1 = Slot(cx, sp - 1, ta);
		if (ta == T_REF || ta == T_PTR || ta == T_VT) FAIL("conversion of a reference");
		if (cop == 0x6b || cop == 0x6c) {              // conv.r4 / conv.r8
			tr = cop == 0x6b ? T_F32 : T_F64;
			LGET(s1);
			if (ta == T_I32) OP(tr == T_F32 ? 0xb2 : 0xb7);
			else if (ta == T_I64) OP(tr == T_F32 ? 0xb4 : 0xb9);
			else if (ta == T_F64 && tr == T_F32) OP(0xb6);
			else if (ta == T_F32 && tr == T_F64) OP(0xbb);
			LSET(Slot(cx, sp - 1, tr));
		} else if (cop == 0x6a) {                      // conv.i8
			tr = T_I64;
			if (ta == T_F32 || ta == T_F64) EmitFloatToInt(cx, ta, T_I64, s1);
			else { LGET(s1); if (ta == T_I32) OP(0xac); }
			LSET(Slot(cx, sp - 1, tr));
		} else if (cop == 0x6e) {                      // conv.u8
			tr = T_I64;
			if (ta == T_F32 || ta == T_F64) FAIL("float to unsigned long");
			LGET(s1); if (ta == T_I32) OP(0xad);
			LSET(Slot(cx, sp - 1, tr));
		} else {                                       // conv.i1 i2 i4 u4 u2 u1 i u : to a 32-bit int
			tr = T_I32;
			if (cop == 0x6d || cop == 0xe0) { if (ta == T_F32 || ta == T_F64) FAIL("float to unsigned"); }
			if (ta == T_F32 || ta == T_F64) EmitFloatToInt(cx, ta, T_I32, s1);
			else { LGET(s1); if (ta == T_I64) OP(0xa7); }
			if (cop == 0x67) OP(0xc0);
			else if (cop == 0x68) OP(0xc1);
			else if (cop == 0xd1) { I32Const(cx, 0xffff); OP(0x71); }
			else if (cop == 0xd2) { I32Const(cx, 0xff); OP(0x71); }
			LSET(Slot(cx, sp - 1, tr));
		}
		cx->st[sp - 1] = (U8)tr;
		break; }

	case 0xfe01: case 0xfe02: case 0xfe03: case 0xfe04: case 0xfe05: {     // ceq cgt cgt.un clt clt.un
		static const int which[6] = { 0, 0, 2, 7, 4, 9 };
		POPN(2); ta = cx->st[sp - 2]; tb = cx->st[sp - 1];
		if ((ta == T_PTR && cx->stRef[sp - 2].mode == R_SCALAR) || (tb == T_PTR && cx->stRef[sp - 1].mode == R_SCALAR)) FAIL("comparison of a pointer to a struct held in locals");
		LGET(Slot(cx, sp - 2, ta)); LGET(Slot(cx, sp - 1, tb));
		if (!EmitCompare(cx, which[in->op - 0xfe00], ta, tb)) return 0;
		LSET(Slot(cx, sp - 2, T_I32));
		cx->sp--; cx->st[cx->sp - 1] = T_I32;
		break; }

	case 0x8e:        // ldlen: the length is the first word of the array object
		POPN(1);
		if (cx->st[sp - 1] != T_REF) FAIL("ldlen of a non-reference");
		s1 = Slot(cx, sp - 1, T_REF);
		LGET(s1); OP(0x45); OP(0x04); OP(0x40); Throw(cx, 1); OP(0x0b);                  // null -> NullReferenceException
		LGET(s1); Mem(cx, 0x28, 0);
		LSET(Slot(cx, sp - 1, T_I32));
		cx->st[sp - 1] = T_I32;
		break;
	case 0x94: case 0x95: case 0x98: case 0x99: case 0x9a: {     // ldelem.i4 .u4 .r4 .r8 .ref
		int rk = (in->op == 0x98) ? T_F32 : (in->op == 0x99) ? T_F64 : (in->op == 0x9a) ? T_REF : T_I32;
		U32 shift = rk == T_F64 ? 3 : 2;
		POPN(2);
		if (cx->st[sp - 2] != T_REF || cx->st[sp - 1] != T_I32) FAIL("bad ldelem operands");
		s1 = Slot(cx, sp - 2, T_REF); s2 = Slot(cx, sp - 1, T_I32);
		LGET(s1); OP(0x45); OP(0x04); OP(0x40); Throw(cx, 1); OP(0x0b);                  // null
		LGET(s2); LGET(s1); Mem(cx, 0x28, 0); OP(0x4f); OP(0x04); OP(0x40); Throw(cx, 2); OP(0x0b);   // index >=u length
		LGET(s1); LGET(s2); I32Const(cx, shift); OP(0x74); OP(0x6a);
		Mem(cx, rk == T_F32 ? 0x2a : rk == T_F64 ? 0x2b : 0x28, 4);
		LSET(Slot(cx, sp - 2, rk));
		cx->sp--; cx->st[cx->sp - 1] = (U8)rk;
		break; }
	case 0x28: {      // call: of an intrinsic, or of a method that is compiled into this module
		if (in->ref != NULL) { if (!EmitCall(cx, in)) return 0; break; }
		int iop = (int)(in->i & 0xff), kind = (int)((in->i >> 8) & 0xff), uns = (int)((in->i >> 16) & 1);
		int binary = (iop == IK_MIN || iop == IK_MAX);
		POPN(binary ? 2 : 1);
		s1 = Slot(cx, sp - (binary ? 2 : 1), kind);
		if (cx->st[sp - 1] != kind || (binary && cx->st[sp - 2] != kind)) FAIL("intrinsic operand type");
		if (kind == T_F32 || kind == T_F64) {
			int f = kind == T_F32;
			LGET(s1);
			if (binary) { LGET(Slot(cx, sp - 1, kind)); OP(iop == IK_MIN ? (f ? 0x96 : 0xa4) : (f ? 0x97 : 0xa5)); }
			else switch (iop) {
				case IK_SQRT: OP(f ? 0x91 : 0x9f); break;
				case IK_ABS: OP(f ? 0x8b : 0x99); break;
				case IK_FLOOR: OP(f ? 0x8e : 0x9c); break;
				case IK_CEIL: OP(f ? 0x8d : 0x9b); break;
				default: OP(f ? 0x8f : 0x9d); break;
			}
		} else {                                           // integer Min / Max: select(a, b, a < b) is the smaller
			int i64 = kind == T_I64;
			U32 lt = i64 ? (uns ? 0x54 : 0x53) : (uns ? 0x49 : 0x48), gt = i64 ? (uns ? 0x56 : 0x55) : (uns ? 0x4b : 0x4a);
			U32 s2b = Slot(cx, sp - 1, kind);
			LGET(s1); LGET(s2b); LGET(s1); LGET(s2b); OP(iop == IK_MIN ? lt : gt); OP(0x1b);
		}
		LSET(s1);
		if (binary) cx->sp--;
		break; }
	case 0x8f: {      // ldelema: the address of an element, of any size
		U32 size = (U32)in->i;
		POPN(2);
		if (cx->st[sp - 2] != T_REF || cx->st[sp - 1] != T_I32) FAIL("bad ldelema operands");
		s1 = Slot(cx, sp - 2, T_REF); s2 = Slot(cx, sp - 1, T_I32);
		LGET(s1); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b);
		LGET(s2); LGET(s1); Mem(cx, 0x28, 0); OP(0x4f); OP(0x04); OP(0x40); Throw(cx, WJ_INDEXRANGE); OP(0x0b);
		LGET(s1); LGET(s2); I32Const(cx, size); OP(0x6c); OP(0x6a); I32Const(cx, 4); OP(0x6a);
		LSET(Slot(cx, sp - 2, T_PTR));
		cx->st[sp - 2] = T_PTR; cx->stTy[sp - 2] = NULL; cx->stRef[sp - 2] = RefMem(Slot(cx, sp - 2, T_PTR), 0);
		cx->sp--;
		break; }
	case 0xa3: case 0xa4: {        // ldelem / stelem of a struct element
		tMD_TypeDef *ty = (tMD_TypeDef*)in->ref; U32 size = ty->arrayElementSize, a, ix, depthArr;
		int isStore = in->op == 0xa4;
		if (KindOfTypeSimple(ty) != T_VT) FAIL("ldelem/stelem of a non-struct");
		POPN(isStore ? 3 : 2);
		depthArr = isStore ? sp - 3 : sp - 2;
		if (cx->st[depthArr] != T_REF || cx->st[depthArr + 1] != T_I32) FAIL("bad element operands");
		if (isStore && (cx->st[sp - 1] != T_VT || cx->stTy[sp - 1] != ty)) FAIL("stelem of a mismatched struct");
		a = Slot(cx, depthArr, T_REF); ix = Slot(cx, depthArr + 1, T_I32);
		LGET(a); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b);
		LGET(ix); LGET(a); Mem(cx, 0x28, 0); OP(0x4f); OP(0x04); OP(0x40); Throw(cx, WJ_INDEXRANGE); OP(0x0b);
		LGET(a); LGET(ix); I32Const(cx, size); OP(0x6c); OP(0x6a); LSET(cx->tmpB);                 // the element, less the 4 bytes of length
		{
			Ref el = RefMem(cx->tmpB, 4);
			if (isStore) { if (!RefCopy(cx, &el, &cx->stRef[sp - 1], ty, size)) return 0; cx->sp -= 3; }
			else { if (!PushVTFrom(cx, depthArr, ty, &el, size)) return 0; cx->sp--; }
		}
		break; }
	case 0x71: case 0x81: case 0xfe15: {        // ldobj / stobj / initobj of a struct
		tMD_TypeDef *ty = (tMD_TypeDef*)in->ref; U32 size = ty->instanceMemSize, ad = (in->op == 0x81) ? sp - 2 : sp - 1;
		Ref target;
		if (KindOfTypeSimple(ty) != T_VT) FAIL("ldobj/stobj/initobj of a non-struct");
		POPN(in->op == 0x81 ? 2 : 1);
		if (!IS_ADDR(cx->st[ad])) FAIL("bad ldobj/stobj/initobj operand");
		if (cx->st[ad] == T_REF) { LGET(Slot(cx, ad, T_REF)); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b); target = RefMem(Slot(cx, ad, T_REF), 0); }
		else target = cx->stRef[ad];
		if (in->op == 0x81) {
			if (cx->st[sp - 1] != T_VT || cx->stTy[sp - 1] != ty) FAIL("bad stobj operands");
			if (!RefCopy(cx, &target, &cx->stRef[sp - 1], ty, size)) return 0;
			cx->sp -= 2;
		} else if (in->op == 0xfe15) { if (!RefZero(cx, &target, ty, size)) return 0; cx->sp--; }
		else if (!PushVTFrom(cx, sp - 1, ty, &target, size)) return 0;
		break; }
	case 0x7b: case 0x7c: {                     // ldfld / ldflda of an instance field of an object, a pointer or a struct value
		tMD_FieldDef *f = (tMD_FieldDef*)in->ref; int fk; tMD_TypeDef *fty; Ref r; U8 kind;
		POPN(1);
		kind = cx->st[sp - 1];
		if (!IS_ADDR(kind) && kind != T_VT) FAIL("ldfld of a non-object");
		if (FIELD_ISSTATIC(f)) FAIL("instance access of a static field");
		if (!FieldInfo(cx, f, &fk, &fty)) return 0;
		if (kind == T_REF) {
			LGET(Slot(cx, sp - 1, T_REF)); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b);
			r = RefMem(Slot(cx, sp - 1, T_REF), 0);
		} else r = cx->stRef[sp - 1];
		if (in->op == 0x7c) {                    // the address of the field
			if (kind == T_VT) FAIL("ldflda of a value");
			if (r.mode == R_SCALAR) { r.off += f->memOffset; cx->stRef[sp - 1] = r; }
			else { LGET(r.addr); I32Const(cx, r.off + f->memOffset); OP(0x6a); LSET(Slot(cx, sp - 1, T_PTR)); cx->stRef[sp - 1] = RefMem(Slot(cx, sp - 1, T_PTR), 0); }
			cx->st[sp - 1] = T_PTR; cx->stTy[sp - 1] = NULL;
		} else if (fk == T_VT) {                 // a struct field: a struct value
			Ref src = r; src.off += f->memOffset;
			if (!PushVTFrom(cx, sp - 1, fty, &src, f->memSize)) return 0;
		} else {
			if (!RefLoad(cx, &r, f->memOffset, fk)) return 0;
			LSET(Slot(cx, sp - 1, fk));
			cx->st[sp - 1] = (U8)fk; cx->stTy[sp - 1] = NULL;
		}
		break; }
	case 0x7d: {                                // stfld
		tMD_FieldDef *f = (tMD_FieldDef*)in->ref; int fk; tMD_TypeDef *fty; Ref r;
		POPN(2);
		if (!IS_ADDR(cx->st[sp - 2])) FAIL("stfld on a non-object");
		if (FIELD_ISSTATIC(f)) FAIL("instance access of a static field");
		if (!FieldInfo(cx, f, &fk, &fty)) return 0;
		if (cx->st[sp - 1] != fk || (fk == T_VT && cx->stTy[sp - 1] != fty)) FAIL("stfld of a mismatched type");
		if (cx->st[sp - 2] == T_REF) {
			LGET(Slot(cx, sp - 2, T_REF)); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b);
			r = RefMem(Slot(cx, sp - 2, T_REF), 0);
		} else r = cx->stRef[sp - 2];
		if (fk == T_VT) { Ref dst = r; dst.off += f->memOffset; if (!RefCopy(cx, &dst, &cx->stRef[sp - 1], fty, f->memSize)) return 0; }
		else if (!RefStore(cx, &r, f->memOffset, fk, Slot(cx, sp - 1, fk))) return 0;
		cx->sp -= 2;
		break; }
	case 0x7e: case 0x80: {                     // ldsfld / stsfld of a primitive static field of a type that is ready
		tMD_FieldDef *f = (tMD_FieldDef*)in->ref; int fk; tMD_TypeDef *fty; tMD_TypeDef *owner;
		if (!FIELD_ISSTATIC(f) || FIELD_ISLITERAL(f) || FIELD_HASFIELDRVA(f) || f->pMemory == NULL) FAIL("static field that is not plain storage");
		if (!FieldInfo(cx, f, &fk, &fty)) return 0;
		if (fk == T_VT) FAIL("static struct field");
		owner = MetaData_GetTypeDefFromFieldDef(f);
		if (!TypeReady(owner) || (f->pParentType != NULL && !TypeReady(f->pParentType))) FAIL("static field of a type that may not be initialised");
		if (in->op == 0x7e) {
			if (sp >= MAX_STACK) FAIL("deep stack");
			I32Const(cx, (I32)(uintptr_t)f->pMemory); Mem(cx, LoadOp(fk), 0); LSET(Slot(cx, sp, fk));
			cx->st[sp] = (U8)fk; cx->stTy[sp] = NULL; cx->sp++;
		} else {
			POPN(1);
			if (cx->st[sp - 1] != fk) FAIL("stsfld of a mismatched type");
			I32Const(cx, (I32)(uintptr_t)f->pMemory); LGET(Slot(cx, sp - 1, fk)); Mem(cx, StoreOp(fk), 0);
			cx->sp--;
		}
		break; }
	case 0x8d: {                                // newarr: the interpreter's, through the C function
		POPN(1);
		if (cx->st[sp - 1] != T_I32) FAIL("newarr of a length that is not an int32");
		I32Const(cx, (I32)(uintptr_t)in->ref); LGET(Slot(cx, sp - 1, T_I32));
		I32Const(cx, (I32)(uintptr_t)&WasmJIT_NewArr); OP(0x11); BU(&cx->body, SIG_NEWARR); OP(0x00);
		LSET(Slot(cx, sp - 1, T_REF));
		LGET(Slot(cx, sp - 1, T_REF)); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_OVERFLOW); OP(0x0b);       // a length that is negative or too large: OverflowException
		cx->st[sp - 1] = T_REF; cx->stTy[sp - 1] = NULL;
		break; }
	case 0x6f: case 0x73: {                     // callvirt (of a method that is not virtual), newobj of a struct
		if (!EmitCall(cx, in)) return 0;
		break; }
	case 0x4a: case 0x4b: case 0x4e: case 0x4f: {     // ldind.i4 .u4 .r4 .r8
		int rk = in->op == 0x4e ? T_F32 : in->op == 0x4f ? T_F64 : T_I32;
		POPN(1);
		if (cx->st[sp - 1] != T_PTR || cx->stRef[sp - 1].mode != R_MEM) FAIL("ldind of a non-pointer");
		LGET(Slot(cx, sp - 1, T_PTR)); Mem(cx, rk == T_F32 ? 0x2a : rk == T_F64 ? 0x2b : 0x28, 0);
		LSET(Slot(cx, sp - 1, rk));
		cx->st[sp - 1] = (U8)rk;
		break; }
	case 0x54: case 0x56: case 0x57: {                // stind.i4 .r4 .r8
		int vk = in->op == 0x56 ? T_F32 : in->op == 0x57 ? T_F64 : T_I32;
		POPN(2);
		if (cx->st[sp - 2] != T_PTR || cx->stRef[sp - 2].mode != R_MEM || cx->st[sp - 1] != vk) FAIL("bad stind operands");
		LGET(Slot(cx, sp - 2, T_PTR)); LGET(Slot(cx, sp - 1, vk));
		Mem(cx, vk == T_F32 ? 0x38 : vk == T_F64 ? 0x39 : 0x36, 0);
		cx->sp -= 2;
		break; }
	case 0xa2: case 0x9e: case 0xa0: case 0xa1: {                // stelem.ref .i4 .r4 .r8 (stelem.ref does no type check here, as in the interpreter)
		int vk = (in->op == 0xa0) ? T_F32 : (in->op == 0xa1) ? T_F64 : (in->op == 0xa2) ? T_REF : T_I32;
		U32 shift = vk == T_F64 ? 3 : 2, sv;
		POPN(3);
		if (cx->st[sp - 3] != T_REF || cx->st[sp - 2] != T_I32 || cx->st[sp - 1] != vk) FAIL("bad stelem operands");
		s1 = Slot(cx, sp - 3, T_REF); s2 = Slot(cx, sp - 2, T_I32); sv = Slot(cx, sp - 1, vk);
		LGET(s1); OP(0x45); OP(0x04); OP(0x40); Throw(cx, 1); OP(0x0b);
		LGET(s2); LGET(s1); Mem(cx, 0x28, 0); OP(0x4f); OP(0x04); OP(0x40); Throw(cx, 2); OP(0x0b);
		LGET(s1); LGET(s2); I32Const(cx, shift); OP(0x74); OP(0x6a);
		LGET(sv);
		Mem(cx, vk == T_F32 ? 0x38 : vk == T_F64 ? 0x39 : 0x36, 4);
		cx->sp -= 3;
		break; }

	default:
		if (IsBranch(in->op)) {
			U32 tgtBlock = cx->blockOf[in->target];
			U32 nextBlock = blockIdx + 1;
			if (in->op == 0x38) {                      // br
				if (cx->structured) break;               // (the structured emitter makes the edge)
				if (!FlowTo(cx, tgtBlock)) return 0;
				if (cx->isEntry && tgtBlock <= blockIdx && cx->sp == 0) EmitYieldCheck(cx, tgtBlock + 1);   // (only the entry function can give up the processor)
				if (tgtBlock != nextBlock) { I32Const(cx, (I32)tgtBlock); LSET(cx->pcLocal); OP(0x0c); BU(&cx->body, loopDepth); }
				*terminated = 1;
				break;
			}
			if (in->op == 0x39 || in->op == 0x3a) {    // brfalse / brtrue
				POPN(1); ta = cx->st[sp - 1];
				if (ta == T_PTR && cx->stRef[sp - 1].mode == R_SCALAR) FAIL("test of a pointer to a struct held in locals");
				LGET(Slot(cx, sp - 1, ta));
				if (ta == T_I64) OP(0x50); else if (ta == T_F32 || ta == T_F64 || ta == T_VT) FAIL("brtrue on a float or struct");
				else OP(0x45);                                              // eqz: true when zero
				if (in->op == 0x3a) OP(0x45);                               // brtrue: invert it
			} else {                                    // beq bge bgt ble blt bne.un bge.un bgt.un ble.un blt.un
				POPN(2); ta = cx->st[sp - 2]; tb = cx->st[sp - 1];
				if ((ta == T_PTR && cx->stRef[sp - 2].mode == R_SCALAR) || (tb == T_PTR && cx->stRef[sp - 1].mode == R_SCALAR)) FAIL("comparison of a pointer to a struct held in locals");
				LGET(Slot(cx, sp - 2, ta)); LGET(Slot(cx, sp - 1, tb));
				// (beq bge bgt ble blt bne.un bge.un bgt.un ble.un blt.un are 0x3b..0x44, in EmitCompare's order)
				if (!EmitCompare(cx, (int)(in->op - 0x3b), ta, tb)) return 0;
				cx->sp--;
			}
			cx->sp--;
			if (cx->structured) break;                  // (the condition is on the wasm stack; the structured emitter makes the two edges)
			if (!FlowTo(cx, tgtBlock) || !FlowTo(cx, nextBlock < cx->nBlocks ? nextBlock : tgtBlock)) return 0;
			OP(0x04); OP(0x40);                                              // if
			if (tgtBlock == nextBlock) { /* the branch is a fall-through */ }
			else {
				if (cx->isEntry && tgtBlock <= blockIdx && cx->sp == 0) EmitYieldCheck(cx, tgtBlock + 1);   // (only the entry function can give up the processor)
				I32Const(cx, (I32)tgtBlock); LSET(cx->pcLocal); OP(0x0c); BU(&cx->body, loopDepth + 1);
			}
			OP(0x0b);
			break;
		}
		FAIL("unhandled opcode");
	}
	return 1;
}

// ================= structured control flow =================
// How a basic block ends, and where it goes.
#define NSUCC(a, b) ((a)->succStart[(b) + 1] - (a)->succStart[(b)])
#define SUCC(a, b, k) ((a)->succ[(a)->succStart[b] + (k)])
enum { BE_RET, BE_BR, BE_COND, BE_FALL, BE_SWITCH };
static int BlockEnd(Fn *cx, U32 b, U32 *s0, U32 *s1) {
	Ins *last = &cx->ins[cx->blockStart[b + 1] - 1];
	if (last->op == 0x2a) return BE_RET;
	if (last->op == 0x45) return BE_SWITCH;
	if (last->op == 0x38) { *s0 = cx->blockOf[last->target]; return BE_BR; }
	if (IsBranch(last->op)) { *s0 = cx->blockOf[last->target]; *s1 = b + 1; return BE_COND; }
	*s0 = b + 1;
	return BE_FALL;
}

static void FreeAnalysis(Analysis *a) {
	if (a == NULL) return;
	free(a->order); free(a->rpo); free(a->idom); free(a->succStart); free(a->succ);
	free(a->isHeader); free(a->isMerge); free(a->outer); free(a->kidStart); free(a->kids); free(a);
}

// The blocks reachable from `root`, their dominators and loops. NULL if the graph cannot be emitted as structured code: a block that falls
// off the end of the method, or a loop with more than one way in (irreducible), which only the dispatch loop can do.
static Analysis *Analyze(Fn *cx, U32 root) {
	U32 n = cx->nBlocks, i, k, nPost = 0, sp = 0;
	Analysis *a = (Analysis*)calloc(1, sizeof(Analysis));
	U32 *post = (U32*)malloc((n + 1) * sizeof(U32)), *stkNode = (U32*)malloc((n + 1) * sizeof(U32)), *stkIdx = (U32*)malloc((n + 1) * sizeof(U32));
	U32 *predStart = (U32*)calloc(n + 2, sizeof(U32)), *preds = NULL, *fill = (U32*)calloc(n + 1, sizeof(U32)), *fwdIn = (U32*)calloc(n + 1, sizeof(U32));
	U8 *seen = (U8*)calloc(n + 1, 1), *body = (U8*)malloc(n + 1);
	int changed;
	a->n = n;
	a->order = (U32*)malloc((n + 1) * sizeof(U32)); a->rpo = (int*)malloc((n + 1) * sizeof(int)); a->idom = (int*)malloc((n + 1) * sizeof(int));
	a->succStart = (U32*)calloc(n + 2, sizeof(U32)); a->succ = NULL;
	a->isHeader = (U8*)calloc(n + 1, 1); a->isMerge = (U8*)calloc(n + 1, 1); a->outer = (U8*)calloc(n + 1, 1);
	a->kidStart = (U32*)calloc(n + 2, sizeof(U32));
	for (i = 0; i < n; i++) {                                                  // the successors of each block
		U32 s0 = 0, s1 = 0, cnt = 0; int e = BlockEnd(cx, i, &s0, &s1);
		a->rpo[i] = -1; a->idom[i] = -1;
		if (e == BE_SWITCH) cnt = (U32)cx->ins[cx->blockStart[i + 1] - 1].i + 1;
		else if (e == BE_RET) cnt = 0;
		else if (e == BE_COND && s0 != s1) cnt = 2;
		else cnt = 1;
		a->succStart[i + 1] = a->succStart[i] + cnt;
	}
	a->succ = (U32*)malloc((a->succStart[n] + 1) * sizeof(U32));
	for (i = 0; i < n; i++) {
		U32 s0 = 0, s1 = 0, k0 = a->succStart[i]; int e = BlockEnd(cx, i, &s0, &s1);
		if (e == BE_SWITCH) {
			Ins *sw = &cx->ins[cx->blockStart[i + 1] - 1]; U32 j;
			for (j = 0; j < (U32)sw->i; j++) a->succ[k0 + j] = cx->blockOf[sw->sw[j]];
			a->succ[k0 + (U32)sw->i] = i + 1;                                  // (not taken: the next block)
		} else if (e == BE_COND && s0 != s1) { a->succ[k0] = s0; a->succ[k0 + 1] = s1; }
		else if (e != BE_RET) a->succ[k0] = s0;
		for (k = k0; k < a->succStart[i + 1]; k++) if (a->succ[k] >= n) goto bad;        // falls off the end
	}
	// depth-first search from the root, for the postorder
	stkNode[0] = root; stkIdx[0] = 0; sp = 1; seen[root] = 1;
	while (sp > 0) {
		U32 b = stkNode[sp - 1];
		if (stkIdx[sp - 1] < NSUCC(a, b)) {
			U32 t = SUCC(a, b, stkIdx[sp - 1]++);
			if (!seen[t]) { seen[t] = 1; stkNode[sp] = t; stkIdx[sp] = 0; sp++; }
		} else { post[nPost++] = b; sp--; }
	}
	a->nOrder = nPost;
	for (i = 0; i < nPost; i++) { a->order[i] = post[nPost - 1 - i]; a->rpo[a->order[i]] = (int)i; }
	// predecessors (of the reachable blocks)
	for (i = 0; i < nPost; i++) { U32 b = a->order[i]; for (k = 0; k < NSUCC(a, b); k++) predStart[SUCC(a, b, k) + 1]++; }
	for (i = 0; i < n; i++) predStart[i + 1] += predStart[i];
	preds = (U32*)malloc((predStart[n] + 1) * sizeof(U32));
	for (i = 0; i < nPost; i++) { U32 b = a->order[i]; for (k = 0; k < NSUCC(a, b); k++) { U32 t = SUCC(a, b, k); preds[predStart[t] + fill[t]++] = b; } }
	// dominators (Cooper, Harvey and Kennedy)
	a->idom[root] = (int)root;
	do {
		changed = 0;
		for (i = 1; i < nPost; i++) {
			U32 b = a->order[i]; int nd = -1;
			for (k = predStart[b]; k < predStart[b + 1]; k++) {
				U32 p = preds[k];
				if (a->idom[p] < 0) continue;
				if (nd < 0) nd = (int)p;
				else {
					int x = (int)p, y = nd;
					while (x != y) { while (a->rpo[x] > a->rpo[y]) x = a->idom[x]; while (a->rpo[y] > a->rpo[x]) y = a->idom[y]; }
					nd = x;
				}
			}
			if (nd != a->idom[b]) { a->idom[b] = nd; changed = 1; }
		}
	} while (changed);
	// the edges: a retreating edge must go to a block that dominates its source (else the graph is irreducible); the others are forward edges
	for (i = 0; i < nPost; i++) {
		U32 b = a->order[i];
		for (k = 0; k < NSUCC(a, b); k++) {
			U32 t = SUCC(a, b, k);
			if (a->rpo[t] <= a->rpo[b]) {
				int x = (int)b;
				while (x != (int)t && x != (int)root) x = a->idom[x];
				if (x != (int)t) goto bad;                                          // irreducible
				a->isHeader[t] = 1;
			} else fwdIn[t]++;
		}
	}
	for (i = 0; i < nPost; i++) if (fwdIn[a->order[i]] >= 2) a->isMerge[a->order[i]] = 1;
	for (i = 0; i < nPost; i++) {                // a switch's targets are branched to by one br_table, which cannot enter code inline: all are merge points, and all forward
		U32 b = a->order[i];
		if (cx->ins[cx->blockStart[b + 1] - 1].op != 0x45) continue;
		for (k = 0; k < NSUCC(a, b); k++) { U32 t = SUCC(a, b, k); if (a->rpo[t] <= a->rpo[b]) goto bad; a->isMerge[t] = 1; }
	}
	// the merge nodes each block immediately dominates, in reverse postorder
	for (i = 1; i < nPost; i++) if (a->isMerge[a->order[i]]) a->kidStart[a->idom[a->order[i]] + 1]++;
	for (i = 0; i < n; i++) a->kidStart[i + 1] += a->kidStart[i];
	a->kids = (U32*)malloc((a->kidStart[n] + 1) * sizeof(U32));
	memset(fill, 0, (n + 1) * sizeof(U32));
	for (i = 1; i < nPost; i++) { U32 v = a->order[i]; if (a->isMerge[v]) { U32 d = (U32)a->idom[v]; a->kids[a->kidStart[d] + fill[d]++] = v; } }
	// the loops: the outermost are the headers that are in no other loop's body
	for (i = 0; i < n; i++) if (a->isHeader[i]) a->outer[i] = 1;
	for (i = 0; i < n; i++) {
		U32 wl, nw = 0, *work;
		if (!a->isHeader[i]) continue;
		memset(body, 0, n + 1); body[i] = 1;
		work = stkNode;
		for (k = 0; k < nPost; k++) {          // each retreating edge into i starts a walk backwards to i
			U32 u = a->order[k], j;
			for (j = 0; j < NSUCC(a, u); j++) if (SUCC(a, u, j) == i && a->rpo[i] <= a->rpo[u] && !body[u]) { body[u] = 1; work[nw++] = u; }
		}
		wl = 0;
		while (wl < nw) { U32 b = work[wl++], q; for (q = predStart[b]; q < predStart[b + 1]; q++) if (!body[preds[q]]) { body[preds[q]] = 1; if (nw < n) work[nw++] = preds[q]; } }
		for (k = 0; k < n; k++) if (k != i && body[k] && a->isHeader[k]) a->outer[k] = 0;
	}
	for (i = 0; i < n; i++) if (a->outer[i]) a->anyOuter = 1;
	free(post); free(stkNode); free(stkIdx); free(predStart); free(preds); free(fill); free(fwdIn); free(seen); free(body);
	return a;
bad:
	free(post); free(stkNode); free(stkIdx); free(predStart); free(preds); free(fill); free(fwdIn); free(seen); free(body);
	FreeAnalysis(a);
	return NULL;
}

static void PushFrame(Fn *cx, int kind, U32 id) {
	if (cx->nFrames == cx->capFrames) { cx->capFrames = cx->capFrames ? cx->capFrames * 2 : 32; cx->frames = realloc(cx->frames, cx->capFrames * sizeof(cx->frames[0])); }
	cx->frames[cx->nFrames].kind = (U8)kind; cx->frames[cx->nFrames].id = id; cx->nFrames++;
}
// how many labels up the frame is (what a `br` to it takes), or -1
static int FrameDepth(Fn *cx, int kind, U32 id) {
	U32 i;
	for (i = cx->nFrames; i > 0; i--) if (cx->frames[i - 1].kind == kind && cx->frames[i - 1].id == id) return (int)(cx->nFrames - i);
	return -1;
}
static U32 LoopIdFor(Fn *cx, U32 header) {
	Fn *m = cx->master ? cx->master : cx;
	if (m->loopId[header] == 0) { m->loopHeaders[m->nLoopIds] = header; m->loopId[header] = ++m->nLoopIds; }
	return m->loopId[header];
}

static int DoTree(Fn *cx, U32 x);
// the stack a block is entered with: what the edges into it said, or empty if no edge has been emitted (a loop header's first edge is the forward one)
static void SetEntryState(Fn *cx, U32 b) {
	U32 i;
	cx->sp = 0;
	if (cx->entryKnown[b]) { cx->sp = cx->entryN[b]; for (i = 0; i < cx->sp; i++) { cx->st[i] = cx->entryT[b][i]; cx->stTy[i] = cx->entryTy[b][i]; } RestoreRefs(cx); }
	else { cx->entryKnown[b] = 1; cx->entryN[b] = 0; }
}
// the edge from block `from` to block `to`: a continue (back to a loop, giving up the processor if it is an outermost loop and the budget is
// spent), a branch to a block that follows (a merge), or, for a block that only this edge reaches, the block itself, right here
static int DoBranch(Fn *cx, U32 from, U32 to) {
	Analysis *a = cx->an;
	if (!FlowTo(cx, to)) return 0;
	if (a->rpo[to] <= a->rpo[from]) {
		int d = FrameDepth(cx, FR_LOOP, to);
		if (d < 0) FAIL("structured code: a loop that is not open");
		if (cx->isEntry && a->outer[to] && cx->sp == 0) EmitYieldCheck(cx, LoopIdFor(cx, to));
		OP(0x0c); BU(&cx->body, (U32)d);
		return 1;
	}
	if (a->isMerge[to]) {
		int d = FrameDepth(cx, FR_BLOCK, to);
		if (d < 0) FAIL("structured code: a merge that no block ends at");
		OP(0x0c); BU(&cx->body, (U32)d);
		return 1;
	}
	return DoTree(cx, to);
}
// the code of x, after the blocks that end at merge points ys (reverse postorder: the last is the farthest, so the outermost wasm block)
static int NodeWithin(Fn *cx, U32 x, const U32 *ys, U32 nys) {
	if (nys == 0) {
		U32 k, s0 = 0, s1 = 0;
		int terminated = 0, e;
		SetEntryState(cx, x);
		for (k = cx->blockStart[x]; k < cx->blockStart[x + 1]; k++) {
			if (!EmitIns(cx, k, x, &terminated)) return 0;
			if (cx->failed) return 0;
		}
		if (terminated) return 1;                                        // a return
		e = BlockEnd(cx, x, &s0, &s1);
		if (e == BE_SWITCH) {
			Ins *sw = &cx->ins[cx->blockStart[x + 1] - 1];
			U32 cnt = (U32)sw->i, j;
			for (j = 0; j < cnt; j++) if (!FlowTo(cx, cx->blockOf[sw->sw[j]])) return 0;
			if (!FlowTo(cx, x + 1)) return 0;
			OP(0x0e); BU(&cx->body, cnt);                                 // (the index was left on the wasm stack by the switch instruction)
			for (j = 0; j <= cnt; j++) {
				int d = FrameDepth(cx, FR_BLOCK, j < cnt ? cx->blockOf[sw->sw[j]] : x + 1);
				if (d < 0) FAIL("structured code: a switch target that no block ends at");
				BU(&cx->body, (U32)d);
				if (j == cnt - 1 && cnt == 0) { }
			}
			return 1;
		}
		if (e == BE_BR || e == BE_FALL) return DoBranch(cx, x, s0);
		if (s0 == s1) { OP(0x1a); return DoBranch(cx, x, s0); }
		{
			U32 savedSp = cx->sp, i; U8 savedSt[MAX_STACK]; tMD_TypeDef *savedTy[MAX_STACK]; Ref *savedRef = (Ref*)malloc((savedSp + 1) * sizeof(Ref));
			for (i = 0; i < savedSp; i++) { savedSt[i] = cx->st[i]; savedTy[i] = cx->stTy[i]; savedRef[i] = cx->stRef[i]; }
			OP(0x04); OP(0x40); PushFrame(cx, FR_IF, 0);
			if (!DoBranch(cx, x, s0)) { free(savedRef); return 0; }
			cx->sp = savedSp; for (i = 0; i < savedSp; i++) { cx->st[i] = savedSt[i]; cx->stTy[i] = savedTy[i]; cx->stRef[i] = savedRef[i]; }
			free(savedRef);
			OP(0x05);
			if (!DoBranch(cx, x, s1)) return 0;
			OP(0x0b); cx->nFrames--;
		}
		return 1;
	} else {
		U32 y = ys[nys - 1];
		OP(0x02); OP(0x40); PushFrame(cx, FR_BLOCK, y);
		if (!NodeWithin(cx, x, ys, nys - 1)) return 0;
		OP(0x0b); cx->nFrames--;
		return DoTree(cx, y);
	}
}
static int DoTree(Fn *cx, U32 x) {
	Analysis *a = cx->an;
	const U32 *ys = a->kids + a->kidStart[x];
	U32 nys = a->kidStart[x + 1] - a->kidStart[x];
	if (a->isHeader[x]) {
		int ok;
		OP(0x03); OP(0x40); PushFrame(cx, FR_LOOP, x);
		ok = NodeWithin(cx, x, ys, nys);
		OP(0x0b); cx->nFrames--;
		return ok;
	}
	return NodeWithin(cx, x, ys, nys);
}

// after the body: the frame size (which the calls pass on to their callees: patched in), and how much scratch this function and its callees need
static int FinishFn(Fn *cx) {
	U32 i, deepest = 0;
	cx->frameSize = (cx->scratchTop + 7) & ~7u;
	for (i = 0; i < cx->nPatches; i++) {
		U8 *p = cx->body.p + cx->patches[i];
		p[0] = (U8)((cx->frameSize & 0x7f) | 0x80); p[1] = (U8)(((cx->frameSize >> 7) & 0x7f) | 0x80);
		p[2] = (U8)(((cx->frameSize >> 14) & 0x7f) | 0x80); p[3] = (U8)(((cx->frameSize >> 21) & 0x7f) | 0x80); p[4] = 0;
	}
	for (i = 0; i < cx->nCallees; i++) if (cx->callees[i]->need > deepest) deepest = cx->callees[i]->need;
	cx->need = cx->frameSize + deepest;
	return 1;
}
static int SetupWasmLocals(Fn *cx) {
	U32 i, sretCount = (cx->hasRet && cx->retKind == T_VT) ? 1 : 0;
	cx->numWasmLocals = 0;
	if (cx->isEntry) cx->sbLocal = NewLocal(cx, T_I32);          // (a function that is called has its scratch pointer as its first parameter)
	for (i = 0; i < cx->nArgs; i++) {
		cx->argL[i] = cx->isEntry ? NewLocal(cx, cx->argT[i]) : 1 + sretCount + i;
		if (cx->argT[i] == T_VT || cx->argT[i] == T_PTR) cx->argRef[i] = RefMem(cx->argL[i], 0);          // (an address)
	}
	for (i = 0; i < cx->nLocals; i++) {
		if (cx->locT[i] != T_VT) { cx->locL[i] = NewLocal(cx, cx->locT[i]); continue; }
		{
			const Leaves *L = LeavesOf(cx->locTy[i]);
			if (L != NULL) {                                         // held in locals, one per leaf (its home in the scratch area is for giving up the processor)
				Ref r; U32 k;
				memset(&r, 0, sizeof(r)); r.mode = R_SCALAR; r.L = L;
				for (k = 0; k < L->n; k++) r.bank[k] = NewLocal(cx, L->kind[k]);
				r.hasFrame = 1; r.frameHome = cx->m->parameterStackSize + cx->locOff[i];
				cx->locRef[i] = r;
			} else if (cx->isEntry) cx->locRef[i] = RefMem(0, cx->m->parameterStackSize + cx->locOff[i]);   // (wasm local 0 is the frame)
			else cx->locRef[i] = RefMem(cx->sbLocal, cx->locHome[i]);
		}
	}
	return !cx->failed;
}

// A function (the entry function, a variant of it, or a callee) with structured control flow, starting at block `root`. A variant for an outermost
// loop starts at that loop's header with its arguments and locals loaded from the frame: that is where a function that gave up the processor is
// called again.
static int EmitStructuredFn(Fn *cx, U32 root) {
	U32 i;
	cx->an = Analyze(cx, root);
	if (cx->an == NULL) FAIL("structured code: not reducible");
	cx->structured = 1;
	cx->tmpA = NewLocal(cx, T_I32);
	cx->tmpB = NewLocal(cx, T_I32);
	if (cx->isEntry) {
		I32Const(cx, 0); OP(0x24); BU(&cx->body, 0);                           // the module's status global: no exception yet
		LGET(0); I32Const(cx, (I32)cx->entryBase); OP(0x6a); LSET(cx->sbLocal);
		FrameAll(cx, 0, 1, root != 0 ? 1 : 0);
	} else {
		for (i = 0; i < cx->nLocals; i++) if (cx->locT[i] == T_VT) { Addr ad = { cx->sbLocal, cx->locHome[i] }; ZeroBytes(cx, ad, cx->locTy[i]->stackSize); }
	}
	if (!DoTree(cx, root)) return 0;
	if (cx->failed) return 0;
	OP(0x00);                                                                  // unreachable
	OP(0x0b);                                                                  // end function
	if (cx->numWasmLocals >= sizeof(cx->localKinds) - 1) FAIL("too many locals");
	FreeAnalysis(cx->an); cx->an = NULL;
	return FinishFn(cx);
}

static int EmitBody(Fn *cx) {
	U32 b, i;
	cx->pcLocal = NewLocal(cx, T_I32);
	cx->tmpA = NewLocal(cx, T_I32);
	cx->tmpB = NewLocal(cx, T_I32);
	if (cx->isEntry) {
		I32Const(cx, 0); OP(0x24); BU(&cx->body, 0);                           // the module's status global: no exception yet
		LGET(0); I32Const(cx, (I32)cx->entryBase); OP(0x6a); LSET(cx->sbLocal);      // the scratch area follows the frame's parameters and locals
		// the arguments come from the frame; and if this is a resumption (entry != 0) so do the locals, and the dispatch starts at the block named
		FrameAll(cx, 0, 1, 0);
		if (cx->hasLoop) {
			LGET(2); OP(0x04); OP(0x40);
			LGET(2); I32Const(cx, 1); OP(0x6b); LSET(cx->pcLocal);
			FrameAll(cx, 0, 0, 1);
			OP(0x0b);
		}
	} else {
		// (the entry function's frame was zeroed when the interpreter made it; the scratch area of a callee has been used before)
		for (i = 0; i < cx->nLocals; i++) if (cx->locT[i] == T_VT) { Addr a = { cx->sbLocal, cx->locHome[i] }; ZeroBytes(cx, a, cx->locTy[i]->stackSize); }
	}
	if (cx->nBlocks > 1 || cx->hasLoop) {
		OP(0x03); OP(0x40);                                                  // loop
		for (b = 0; b < cx->nBlocks; b++) { OP(0x02); OP(0x40); }            // block x N
		LGET(cx->pcLocal);
		OP(0x0e); BU(&cx->body, cx->nBlocks);
		for (b = 0; b < cx->nBlocks; b++) BU(&cx->body, b);
		BU(&cx->body, cx->nBlocks - 1);
	}
	for (b = 0; b < cx->nBlocks; b++) {
		U32 k;
		int terminated = 0;
		if (cx->nBlocks > 1 || cx->hasLoop) OP(0x0b);                        // end of block B_b: its code starts here
		// the stack on entry
		cx->sp = 0;
		if (cx->entryKnown[b]) { cx->sp = cx->entryN[b]; for (i = 0; i < cx->sp; i++) { cx->st[i] = cx->entryT[b][i]; cx->stTy[i] = cx->entryTy[b][i]; } RestoreRefs(cx); }
		else { cx->entryKnown[b] = 1; cx->entryN[b] = 0; }
		for (k = cx->blockStart[b]; k < cx->blockStart[b + 1]; k++) {
			if (!EmitIns(cx, k, b, &terminated)) return 0;
			if (cx->failed) return 0;
		}
		if (!terminated && b + 1 < cx->nBlocks) { if (!FlowTo(cx, b + 1)) return 0; }   // falls into the next block
		if (!terminated && b + 1 == cx->nBlocks) FAIL("falls off the end of the method");
	}
	if (cx->numWasmLocals >= sizeof(cx->localKinds) - 1) FAIL("too many locals");
	if (cx->nBlocks > 1 || cx->hasLoop) OP(0x0b);                            // end loop
	OP(0x00);                                                                // unreachable
	OP(0x0b);                                                                // end function
	return 1;
}

// the method's body: where its CIL is and its locals (JIT_Prepare does the same for the interpreter, from the same metadata)
static int LoadBody(tMD_MethodDef *m, const U8 **pCil, U32 *pSize, tParameter **pLocals, U32 *pNumLocals) {
	U8 *h = (U8*)m->pCIL;
	U32 numLocals = 0;
	IDX_TABLE localsToken;
	U16 flags;
	tParameter *locals = NULL;
	if (h == NULL) return 0;
	if ((*h & 0x3) == 0x2) {                       // tiny header
		flags = *h & 0x3; *pSize = (*h & 0xfc) >> 2; localsToken = 0; *pCil = h + 1;
	} else {
		flags = *(U16*)h & 0x0fff; *pSize = *(U32*)&h[4]; localsToken = *(IDX_TABLE*)&h[8]; *pCil = h + ((h[1] & 0xf0) >> 2);
	}
	if (flags & 0x08) return 0;                    // exception clauses: not compiled
	if (localsToken != 0) {
		tMD_StandAloneSig *pSig = (tMD_StandAloneSig*)MetaData_GetTableRow(m->pMetaData, localsToken);
		U32 sigLength, i, total = 0;
		SIG sig = MetaData_GetBlob(pSig->signature, &sigLength);
		MetaData_DecodeSigEntry(&sig);             // always 0x07
		numLocals = MetaData_DecodeSigEntry(&sig);
		locals = (tParameter*)malloc((numLocals + 1) * sizeof(tParameter));
		for (i = 0; i < numLocals; i++) {
			tMD_TypeDef *t = Type_GetTypeFromSig(m->pMetaData, &sig, m->pParentType->ppClassTypeArgs, m->ppMethodTypeArgs);
			MetaData_Fill_TypeDef(t, NULL, NULL);
			locals[i].pTypeDef = t; locals[i].offset = total; locals[i].size = t->stackSize;
			total += t->stackSize;
		}
	}
	*pLocals = locals; *pNumLocals = numLocals;
	return 1;
}

static void FreeFn(Fn *cx) {
	if (!cx->isClone) { free(cx->descs); free(cx->locParams); }
	if (!cx->isClone) { U32 k; for (k = 0; k < cx->n; k++) if (cx->ins[k].op == 0x45) free(cx->ins[k].sw); }
	if (cx->isClone) {                                  // (what it shares is the master's)
		free(cx->entryKnown); free(cx->entryT); free(cx->entryTy); free(cx->entryN); free(cx->argL); free(cx->locL); free(cx->stRef); free(cx->argRef); free(cx->locRef);
		free(cx->patches); free(cx->callees); free(cx->body.p); free(cx->frames); FreeAnalysis(cx->an); free(cx);
		return;
	}
	free(cx->loopId); free(cx->loopHeaders); free(cx->variantFn); free(cx->frames); FreeAnalysis(cx->an); free(cx->stRef); free(cx->argRef); free(cx->locRef);
	free(cx->ins); free(cx->insAt); free(cx->blockOf); free(cx->blockStart); free(cx->entryKnown); free(cx->entryT); free(cx->entryTy); free(cx->entryN);
	free(cx->argT); free(cx->argN); free(cx->argL); free(cx->argTy); free(cx->locT); free(cx->locN); free(cx->locL); free(cx->locTy); free(cx->locOff);
	free(cx->locHome); free(cx->patches); free(cx->callees); free(cx->body.p); free(cx);
}

// A copy of the entry function that starts at `root`: it shares what was decoded, and has its own code, locals, temporaries and edges.
static Fn *CloneVariant(Fn *m, U32 root) {
	Fn *v;
	if (m->mod->nFns >= MAX_FNS) { m->why = "too many functions"; return NULL; }
	v = (Fn*)malloc(sizeof(Fn));
	*v = *m;
	v->isClone = 1; v->isDispatcher = 0; v->master = m; v->variantRoot = root; v->inProgress = 1;
	memset(&v->body, 0, sizeof(Buf));
	v->numWasmLocals = 0; memset(v->slot, 0xff, sizeof(v->slot)); memset(v->nTmp, 0, sizeof(v->nTmp));
	v->scratchTop = m->homesTop; v->frameSize = 0; v->need = 0;
	v->patches = NULL; v->nPatches = v->capPatches = 0; v->callees = NULL; v->nCallees = 0; v->canThrow = 0; v->failed = 0; v->why = NULL; v->sp = 0;
	v->an = NULL; v->frames = NULL; v->nFrames = v->capFrames = 0;
	v->loopId = v->loopHeaders = v->variantFn = NULL; v->nLoopIds = v->nVariantFns = 0;
	v->entryKnown = (U8*)calloc(m->nBlocks + 1, 1); v->entryT = (U8 (*)[MAX_STACK])calloc(m->nBlocks + 1, MAX_STACK);
	v->entryTy = (tMD_TypeDef *(*)[MAX_STACK])calloc(m->nBlocks + 1, MAX_STACK * sizeof(tMD_TypeDef*)); v->entryN = (U32*)calloc(m->nBlocks + 1, sizeof(U32));
	v->argL = (U32*)calloc(m->nArgs + 1, sizeof(U32)); v->locL = (U32*)calloc(m->nLocals + 1, sizeof(U32));
	v->stRef = (Ref*)calloc(MAX_STACK, sizeof(Ref)); v->argRef = (Ref*)calloc(m->nArgs + 1, sizeof(Ref)); v->locRef = (Ref*)calloc(m->nLocals + 1, sizeof(Ref));
	memset(v->nBank, 0, sizeof(v->nBank));
	v->index = m->mod->nFns; m->mod->fns[m->mod->nFns++] = v;
	return v;
}
static int EmitVariant(Fn *v) {
	if (!SetupWasmLocals(v) || !EmitStructuredFn(v, v->variantRoot)) return 0;
	v->inProgress = 0;
	return 1;
}

// How the function's code is made: structured (wasm loops, blocks and ifs) when its control flow allows, else the dispatch loop. An entry function
// with loops becomes a dispatcher and a function for the start and for each outermost loop that it may be resumed at.
static int EmitCode(Fn *cx) {
	Analysis *probe = (getenv("DNA_WASM_JIT_NOSTRUCT") == NULL && cx->nBlocks <= 3000) ? Analyze(cx, 0) : NULL;
	if (probe == NULL) {                       // irreducible, or switched off: the dispatch loop
		if (!SetupWasmLocals(cx) || !EmitBody(cx)) return 0;
		return FinishFn(cx);
	}
	if (!(cx->isEntry && probe->anyOuter)) {
		FreeAnalysis(probe);
		return SetupWasmLocals(cx) && EmitStructuredFn(cx, 0);
	}
	FreeAnalysis(probe);
	{
		U32 id; Fn *v;
		cx->isDispatcher = 1; cx->structured = 1;
		cx->loopId = (U32*)calloc(cx->nBlocks + 1, sizeof(U32)); cx->loopHeaders = (U32*)calloc(cx->nBlocks + 1, sizeof(U32)); cx->variantFn = (U32*)calloc(cx->nBlocks + 2, sizeof(U32));
		v = CloneVariant(cx, 0);
		if (v == NULL) return 0;
		cx->variantFn[0] = v->index;
		if (!EmitVariant(v)) { cx->why = v->why; return 0; }
		cx->need = v->need;
		for (id = 1; id <= cx->nLoopIds; id++) {               // (the loops that the code so far can give up the processor at; emitting these may name more)
			v = CloneVariant(cx, cx->loopHeaders[id - 1]);
			if (v == NULL) return 0;
			cx->variantFn[id] = v->index;
			if (!EmitVariant(v)) { cx->why = v->why; return 0; }
			if (v->need > cx->need) cx->need = v->need;
		}
		cx->nVariantFns = cx->nLoopIds + 1;
	}
	return 1;
}

static Fn *CompileFn(Mod *mod, tMD_MethodDef *m, int isEntry, const U8 *cil, U32 size, const tParameter *locals, U32 nLocals, U32 entryBase) {
	static U32 minInstr = 0xffffffff;
	Fn *cx = (Fn*)calloc(1, sizeof(Fn));
	U32 i, anyBack = 0, sretCount = 0;
	int prepOnly;
	if (minInstr == 0xffffffff) minInstr = getenv("DNA_WASM_JIT_MIN") != NULL ? (U32)atoi(getenv("DNA_WASM_JIT_MIN")) : 12;
	cx->mod = mod; cx->m = m; cx->isEntry = isEntry; cx->cil = cil; cx->size = size; cx->entryBase = entryBase; cx->inProgress = 1;
	memset(cx->slot, 0xff, sizeof(cx->slot));
	prepOnly = mod->prepOnly; mod->prepOnly = 0;       // (decode it and stop: to be inlined, not made into a function)
	if (!prepOnly) { cx->index = mod->nFns; mod->fns[mod->nFns++] = cx; mod->depth++; }
	cx->inlBudget = 1500;
	if (!Decode(cx) || !FindBlocks(cx)) goto fail;
	cx->curIns = cx->ins;
	for (i = 0; i < cx->n; i++) {
		if (IsBranch(cx->ins[i].op) && cx->ins[i].target <= i) anyBack = 1;
		if (cx->ins[i].op == 0x45) { U32 j; for (j = 0; j < (U32)cx->ins[i].i; j++) if (cx->ins[i].sw[j] <= i) anyBack = 1; }
	}
	if (isEntry && !anyBack && cx->n < minInstr) { cx->why = "too small to be worth it"; goto fail; }
	cx->hasLoop = anyBack;

	cx->hasRet = m->pReturnType != NULL;
	if (cx->hasRet) {
		U8 nar;
		int k = KindOfType(m->pReturnType, &nar);
		if (k < 0 || (k != T_VT && m->pReturnType->stackSize != SizeOfKind(k))) { cx->why = "unsupported return type"; goto fail; }
		cx->retKind = (U32)k; cx->retTy = m->pReturnType;
		if (k == T_VT) sretCount = 1;
	}
	cx->nArgs = m->numberOfParameters; cx->nLocals = nLocals;
	if (!isEntry && nLocals > 0) { cx->locParams = (tParameter*)malloc(nLocals * sizeof(tParameter)); memcpy(cx->locParams, locals, nLocals * sizeof(tParameter)); }   // (for the interpreter's version of it)
	for (i = 0; i < nLocals; i++) if (locals[i].offset + locals[i].size > cx->localsBytes) cx->localsBytes = locals[i].offset + locals[i].size;
	cx->numParams = isEntry ? 4 : 1 + sretCount + cx->nArgs;
	cx->sbLocal = 0; cx->sretLocal = 1;           // (the entry function makes its own sbLocal)
	cx->argT = (U8*)calloc(cx->nArgs + 1, 1); cx->argN = (U8*)calloc(cx->nArgs + 1, 1); cx->argL = (U32*)calloc(cx->nArgs + 1, sizeof(U32));
	cx->argTy = (tMD_TypeDef**)calloc(cx->nArgs + 1, sizeof(void*));
	cx->locT = (U8*)calloc(nLocals + 1, 1); cx->locN = (U8*)calloc(nLocals + 1, 1); cx->locL = (U32*)calloc(nLocals + 1, sizeof(U32));
	cx->locTy = (tMD_TypeDef**)calloc(nLocals + 1, sizeof(void*)); cx->locOff = (U32*)calloc(nLocals + 1, sizeof(U32)); cx->locHome = (U32*)calloc(nLocals + 1, sizeof(U32));
	for (i = 0; i < cx->nArgs; i++) {
		tMD_TypeDef *t = m->pParams[i].pTypeDef;
		int k;
		if (i == 0 && !METHOD_ISSTATIC(m)) k = m->pParentType->isValueType ? T_PTR : T_REF;       // 'this': a pointer to the struct, or the object
		else k = KindOfType(t, &cx->argN[i]);
		if (k < 0 || k == T_PTR + 100) { cx->why = "unsupported parameter type"; goto fail; }
		if (k == T_VT) cx->argTy[i] = t;
		else if (m->pParams[i].size != SizeOfKind(k)) { cx->why = "unsupported parameter type"; goto fail; }
		cx->argT[i] = (U8)k;
	}
	for (i = 0; i < nLocals; i++) {
		int k = KindOfType(locals[i].pTypeDef, &cx->locN[i]);
		if (k < 0 || (k != T_VT && locals[i].size != SizeOfKind(k))) { cx->why = "unsupported local type"; goto fail; }
		cx->locT[i] = (U8)k; cx->locOff[i] = locals[i].offset;
		if (k == T_VT) { cx->locTy[i] = locals[i].pTypeDef; cx->locHome[i] = AllocScratch(cx, TySize(cx->locTy[i])); }
	}
	cx->stRef = (Ref*)calloc(MAX_STACK, sizeof(Ref)); cx->argRef = (Ref*)calloc(cx->nArgs + 1, sizeof(Ref)); cx->locRef = (Ref*)calloc(nLocals + 1, sizeof(Ref));
	cx->homesTop = cx->scratchTop;                   // (the struct locals' homes come first, so every variant of the function agrees on them)
	if (cx->failed) goto fail;
	if (prepOnly) { cx->inProgress = 0; return cx; }
	if (!EmitCode(cx)) goto fail;

	if (cx->need > (1u << 26)) { cx->why = "frame too large"; goto fail; }
	cx->inProgress = 0; mod->depth--;
	return cx;
fail:
	mod->why = cx->why ? cx->why : "?";
	if (prepOnly) { FreeFn(cx); return NULL; }
	mod->depth--;
	return NULL;
}

// May a method be called at all from compiled code? It needs CIL; a call that is not virtual; and a type whose constructor has not to run first.
static int CalleeOK(Fn *cx, tMD_MethodDef *c, int isVirt) {
	tMD_TypeDef *owner = c->pParentType;
	if (c->pCIL == NULL || (c->implFlags & METHODIMPLATTRIBUTES_INTERNALCALL) || (c->implFlags & METHODIMPLATTRIBUTES_CODETYPE_MASK) == METHODIMPLATTRIBUTES_CODETYPE_RUNTIME) {
		cx->why = "call of a method without CIL"; return 0;
	}
	if (isVirt && (METHOD_ISVIRTUAL(c) || TYPE_ISINTERFACE(owner))) { cx->why = "virtual call"; return 0; }
	// (calling an instance method does not run the type's constructor; a static method, or the constructor of a struct, does)
	if ((METHOD_ISSTATIC(c) || !strcmp((const char*)c->name, ".ctor")) && !TypeReady(owner)) { cx->why = "call into a type that may not be initialised"; return 0; }
	return 1;
}

// A callee that is decoded and typed but not emitted, to be looked at and perhaps inlined. Kept for the module.
static Fn *PrepFn(Mod *mod, tMD_MethodDef *c) {
	const U8 *cil; U32 size, nLocals, i; tParameter *locals; Fn *fn;
	for (i = 0; i < mod->nPreps; i++) if (mod->preps[i]->m == c) return mod->preps[i];
	if (mod->nPreps >= MAX_PREPS) { mod->why = "too many callees"; return NULL; }
	if (!LoadBody(c, &cil, &size, &locals, &nLocals)) { mod->why = "callee body not compilable"; return NULL; }
	mod->prepOnly = 1;
	fn = CompileFn(mod, c, 0, cil, size, locals, nLocals, 0);
	mod->prepOnly = 0;
	free(locals);
	if (fn != NULL) mod->preps[mod->nPreps++] = fn;
	return fn;
}

// Inline it, or call it? A method that is straight-line (no branches, one basic block), short, not in the chain being inlined, whose structs can all
// be held in locals (so that no struct memory is shared between it and us).
// Whether a method makes a virtual call, or calls a method that does (as far as is looked): then it, or what it calls, can give its frame to the interpreter,
// and a method that does cannot be inlined into code that must be able to give its own.
static Fn *PrepFn(Mod *mod, tMD_MethodDef *c);
static int MayDeopt(Mod *mod, tMD_MethodDef *m, int depth) {
	Fn *prep;
	U32 i;
	if (depth > 3) return 0;
	prep = PrepFn(mod, m);
	if (prep == NULL) return 0;
	for (i = 0; i < prep->n; i++) {
		U8 op = prep->ins[i].op;
		tMD_MethodDef *t = (tMD_MethodDef*)prep->ins[i].ref;
		if (op != 0x28 && op != 0x6f && op != 0x73) continue;
		if (op == 0x6f && (METHOD_ISVIRTUAL(t) || TYPE_ISINTERFACE(t->pParentType))) return 1;
		if (t != m && MayDeopt(mod, t, depth + 1)) return 1;
	}
	return 0;
}

static int CanInline(Fn *cx, Fn *prep) {
	static int off = -1;
	U32 i;
	if (off < 0) off = getenv("DNA_WASM_JIT_NOINLINE") != NULL;
	if (off || prep->nBlocks != 1 || prep->n > 80 || cx->inlDepth >= 4 || cx->inlBudget < prep->n) return 0;
	if (prep->m == cx->m) return 0;
	for (i = 0; i < cx->inlDepth; i++) if (cx->inlStack[i] == prep->m) return 0;
	for (i = 0; i < prep->n; i++) {
		if (IsBranch(prep->ins[i].op)) return 0;
		if ((prep->ins[i].op == 0x28 || prep->ins[i].op == 0x6f || prep->ins[i].op == 0x73) && MayDeopt(cx->mod, (tMD_MethodDef*)prep->ins[i].ref, 1)) return 0;
		if (prep->ins[i].op == 0x6f && (METHOD_ISVIRTUAL((tMD_MethodDef*)prep->ins[i].ref) || TYPE_ISINTERFACE(((tMD_MethodDef*)prep->ins[i].ref)->pParentType))) return 0;   // (its deoptimization would be in the middle of ours)
	}
	if (prep->ins[prep->n - 1].op != 0x2a) return 0;
	for (i = 0; i < prep->nArgs; i++) if (prep->argT[i] == T_VT && LeavesOf(prep->argTy[i]) == NULL) return 0;
	for (i = 0; i < prep->nLocals; i++) if (prep->locT[i] == T_VT && LeavesOf(prep->locTy[i]) == NULL) return 0;
	if (prep->hasRet && prep->retKind == T_VT && LeavesOf(prep->retTy) == NULL) return 0;
	return 1;
}

// The function for a method that the one being compiled calls: compiled into the same module (once), or NULL (with the reason).
static Fn *GetCallee(Fn *cx, tMD_MethodDef *c, int isVirt) {
	Mod *mod = cx->mod;
	tMD_TypeDef *owner = c->pParentType;
	const U8 *cil; U32 size, nLocals; tParameter *locals;
	Fn *fn;
	U32 i;
	for (i = 0; i < mod->nFns; i++) if (mod->fns[i]->m == c) {
		if (mod->fns[i]->inProgress || mod->fns[i]->isEntry) { cx->why = "recursive call"; return NULL; }
		return mod->fns[i];
	}
	if (mod->nFns >= MAX_FNS || mod->depth >= MAX_DEPTH) { cx->why = "too many or too deep calls"; return NULL; }
	if (!CalleeOK(cx, c, isVirt)) return NULL;
	if (!LoadBody(c, &cil, &size, &locals, &nLocals)) { cx->why = "callee body not compilable"; return NULL; }
	fn = CompileFn(mod, c, 0, cil, size, locals, nLocals, 0);
	free(locals);
	if (fn == NULL) cx->why = mod->why;
	return fn;
}

// The body of a straight-line callee, emitted here, in place of a call. Its arguments are copied into fresh locals first (so that what it pushes
// on our stack cannot overwrite them); its locals are fresh, and zero; a struct it handles is held in locals. Its ret leaves the result on our
// stack, where the callee's stack began. For newobj the object is a fresh zeroed bank (or the allocated object), passed as `this`.
static int EmitInline(Fn *cx, Fn *prep, int isNew, tMD_TypeDef *newTy, U32 objLocal, U32 base) {
	U32 first = isNew ? 1 : 0, i, k;
	U32 *argL2 = (U32*)calloc(prep->nArgs + 1, sizeof(U32)), *locL2 = (U32*)calloc(prep->nLocals + 1, sizeof(U32));
	Ref *argRef2 = (Ref*)calloc(prep->nArgs + 1, sizeof(Ref)), *locRef2 = (Ref*)calloc(prep->nLocals + 1, sizeof(Ref));
	Ref newRef;
	int ok = 1;
	U8 *sArgT = cx->argT, *sArgN = cx->argN, *sLocT = cx->locT, *sLocN = cx->locN; tMD_TypeDef **sArgTy = cx->argTy, **sLocTy = cx->locTy;
	U32 sNArgs = cx->nArgs, sNLocals = cx->nLocals, *sArgL = cx->argL, *sLocL = cx->locL, *sLocOff = cx->locOff, *sLocHome = cx->locHome;
	Ref *sArgRef = cx->argRef, *sLocRef = cx->locRef; Ins *sIns = cx->curIns;
	typeof(cx->inl) sInl = cx->inl;
	memset(&newRef, 0, sizeof(newRef));
	for (i = 0; i < prep->nArgs && ok; i++) {                       // the arguments, copied
		U8 kind = prep->argT[i];
		if (isNew && i == 0) {
			if (newTy->isValueType) {
				const Leaves *L = LeavesOf(newTy);
				newRef.mode = R_SCALAR; newRef.L = L;
				for (k = 0; k < L->n; k++) newRef.bank[k] = NewLocal(cx, L->kind[k]);
				argRef2[0] = newRef;
			} else { argL2[0] = NewLocal(cx, T_I32); LGET(objLocal); LSET(argL2[0]); }
		} else {
			U32 d = base + (i - first);
			if (kind == T_VT) {
				const Leaves *L = LeavesOf(prep->argTy[i]);
				Ref r; memset(&r, 0, sizeof(r)); r.mode = R_SCALAR; r.L = L;
				for (k = 0; k < L->n; k++) r.bank[k] = NewLocal(cx, L->kind[k]);
				if (!RefCopy(cx, &r, &cx->stRef[d], prep->argTy[i], prep->argTy[i]->stackSize)) ok = 0;
				argRef2[i] = r;
			} else if (kind == T_PTR) {
				Ref src = cx->stRef[d];
				if (src.mode == R_SCALAR) argRef2[i] = src;
				else { U32 a = NewLocal(cx, T_I32); LGET(src.addr); if (src.off) { I32Const(cx, src.off); OP(0x6a); } LSET(a); argRef2[i] = RefMem(a, 0); }
			} else { argL2[i] = NewLocal(cx, kind); LGET(Slot(cx, d, kind)); LSET(argL2[i]); }
		}
	}
	if (ok && isNew && newTy->isValueType) ok = RefZero(cx, &newRef, newTy, TySize(newTy));
	for (i = 0; i < prep->nLocals && ok; i++) {                      // the locals: fresh, and zero (each time this code runs)
		if (prep->locT[i] == T_VT) {
			const Leaves *L = LeavesOf(prep->locTy[i]);
			Ref r; memset(&r, 0, sizeof(r)); r.mode = R_SCALAR; r.L = L;
			for (k = 0; k < L->n; k++) r.bank[k] = NewLocal(cx, L->kind[k]);
			ok = RefZero(cx, &r, prep->locTy[i], prep->locTy[i]->stackSize);
			locRef2[i] = r;
		} else {
			locL2[i] = NewLocal(cx, prep->locT[i]);
			switch (prep->locT[i]) { case T_I64: I64Const(cx, 0); break; case T_F32: F32Const(cx, 0); break; case T_F64: F64Const(cx, 0); break; default: I32Const(cx, 0); }
			LSET(locL2[i]);
		}
	}
	if (ok && !cx->failed) {
		cx->argT = prep->argT; cx->argN = prep->argN; cx->argTy = prep->argTy; cx->nArgs = prep->nArgs; cx->argL = argL2; cx->argRef = argRef2;
		cx->locT = prep->locT; cx->locN = prep->locN; cx->locTy = prep->locTy; cx->nLocals = prep->nLocals; cx->locL = locL2; cx->locRef = locRef2;
		cx->locOff = prep->locOff; cx->locHome = prep->locHome; cx->curIns = prep->ins;
		cx->sp = base;
		cx->inl.active = 1; cx->inl.base = base; cx->inl.hasRet = prep->hasRet; cx->inl.retKind = prep->retKind; cx->inl.retTy = prep->retTy;
		cx->inlStack[cx->inlDepth++] = prep->m; cx->inlBudget -= prep->n;
		for (k = 0; k < prep->n; k++) {
			int term = 0;
			if (!EmitIns(cx, k, 0, &term) || cx->failed) { ok = 0; break; }
			if (term) break;
		}
		cx->inlDepth--;
		cx->argT = sArgT; cx->argN = sArgN; cx->argTy = sArgTy; cx->nArgs = sNArgs; cx->argL = sArgL; cx->argRef = sArgRef;
		cx->locT = sLocT; cx->locN = sLocN; cx->locTy = sLocTy; cx->nLocals = sNLocals; cx->locL = sLocL; cx->locRef = sLocRef;
		cx->locOff = sLocOff; cx->locHome = sLocHome; cx->curIns = sIns; cx->inl = sInl;
	} else ok = 0;
	free(argL2); free(locL2); free(argRef2); free(locRef2);
	if (!ok) return 0;
	if (isNew) {                                                    // the new object is what is left on the stack
		cx->sp = base;
		if (newTy->isValueType) { if (!PushVTFrom(cx, base, newTy, &newRef, TySize(newTy))) return 0; }
		else { LGET(objLocal); LSET(Slot(cx, base, T_REF)); cx->st[base] = T_REF; cx->stTy[base] = NULL; }
		cx->sp = base + 1;
	}
	return 1;
}

// ================= speculative devirtualization =================
// A virtual or interface call in the method's own code (not in a method it calls: that could not hand its frame to the interpreter) is compiled for the
// targets that the types that exist can reach. An object cannot be of a type that is not filled in yet, so the filled concrete types that derive from
// the method's class (or implement its interface) are every type a receiver can have now. Their distinct targets (at most MAXPRED) are guarded for in
// turn -- by comparing the target that the receiver's vtable names, or for an interface its type -- and each is called, or inlined, as an ordinary
// call. A receiver that none of them fits (a type made since, one that was not counted) fails the guard: the state is written for the interpreter,
// and it carries on from the call (WasmJIT.h, tWasmDeopt).
static int WJDebug(void) { static int d = -1; if (d < 0) d = getenv("DNA_WASM_JIT_DEBUG") != NULL; return d; }
#define MAXPRED 6
typedef struct { tMD_MethodDef *target; tMD_TypeDef *type; } Pred;
typedef struct { tMD_MethodDef *m; tMD_TypeDef *decl; int isIface; Pred p[MAXPRED]; int n, overflow; } PredCtx;

typedef struct { tMD_MethodDef *entry, *callee; tMD_TypeDef *type; } Observed;     // a receiver type that failed the guards of a call in a method
static Observed observed[256];
static U32 nObserved;
int WasmJIT_NoteMiss(tMD_MethodDef *pMethod, tMD_MethodDef *callee, tMD_TypeDef *type) {
	U32 i;
	for (i = 0; i < nObserved; i++) if (observed[i].entry == pMethod && observed[i].callee == callee && observed[i].type == type) return 0;
	if (nObserved == sizeof(observed) / sizeof(observed[0])) return 0;
	observed[nObserved].entry = pMethod; observed[nObserved].callee = callee; observed[nObserved].type = type; nObserved++;
	return 1;
}

static void PredVisit(tMD_TypeDef *t, void *vctx) {
	PredCtx *c = (PredCtx*)vctx;
	tMD_MethodDef *target = NULL;
	int k;
	if (c->overflow || !t->isFilled || t->isGenericDefinition || TYPE_ISINTERFACE(t) || (t->flags & 0x80 /* abstract */) || t->pVTable == NULL) return;
	if (!c->isIface) {
		tMD_TypeDef *a = t;
		while (a != NULL && a != c->decl) a = a->pParent;
		if (a == NULL || c->m->vTableOfs >= t->numVirtualMethods) return;
		target = t->pVTable[c->m->vTableOfs];
	} else {
		I32 fi;
		for (fi = (I32)t->numInterfaces - 1; fi >= 0; fi--) {          // (as the interpreter looks: the most recent implementation of the interface wins)
			if (t->pInterfaceMaps[fi].pInterface == c->decl) {
				if (t->pInterfaceMaps[fi].pVTableLookup != NULL) target = t->pVTable[t->pInterfaceMaps[fi].pVTableLookup[c->m->vTableOfs]];
				else target = t->pInterfaceMaps[fi].ppMethodVLookup[c->m->vTableOfs];
				break;
			}
		}
	}
	if (target == NULL) return;
	for (k = 0; k < c->n; k++) if (c->isIface ? c->p[k].type == t : c->p[k].target == target) return;
	if (c->n >= MAXPRED) { c->overflow = 1; return; }
	c->p[c->n].target = target; c->p[c->n].type = t; c->n++;
}

static U32 HeaderMaxStack(tMD_MethodDef *m) {
	const U8 *h = (const U8*)m->pCIL;
	return (*h & 0x3) == 0x2 ? 8 : *(const U16*)&h[2];
}
// The types that the method makes objects of are filled in by the interpreter's translation of it, which is made when compilation is done (the frame goes
// to that version when a guard fails). Prediction wants them now: a receiver can be of any of them.
static void FillTypesUsed(Fn *cx) {
	U32 i;
	if (cx->mod->filledUsed) return;
	cx->mod->filledUsed = 1;
	for (i = 0; i < cx->n; i++) {
		if (cx->ins[i].op == 0x73) {
			tMD_MethodDef *ctor = (tMD_MethodDef*)cx->ins[i].ref;
			if (ctor != NULL && ctor->pParentType != NULL && !ctor->pParentType->isFilled) MetaData_Fill_TypeDef(ctor->pParentType, NULL, NULL);
		}
	}
}
static U32 AddDeoptSite(Fn *cx, U32 ip, U32 bytes, tMD_MethodDef *callee, U32 recvOfs) {
	Mod *mod = cx->mod;
	if (mod->nSites == mod->capSites) { mod->capSites = mod->capSites ? mod->capSites * 2 : 16; mod->sites = (tWasmDeoptSite*)realloc(mod->sites, mod->capSites * sizeof(tWasmDeoptSite)); }
	mod->sites[mod->nSites].ip = ip; mod->sites[mod->nSites].stackBytes = bytes; mod->sites[mod->nSites].callee = callee; mod->sites[mod->nSites].recvOfs = recvOfs;
	if (bytes > mod->maxStackBytes) mod->maxStackBytes = bytes;
	return mod->nSites++;
}
// The state at a place where the interpreter takes over, with the first `limit` entries of the evaluation stack: the interpreter's version of the method
// goes on at the CIL instruction `cilOfs` (see tWasmDeopt). The entry function writes its locals to the frame and the stack from `sp` up, and returns
// WJ_DEOPT + site. A function that is called writes an image of its frame (tWasmFrameDesc) in its scratch area, and pushes it on the chain: if this is a guard
// failing here (isMiss), it then returns with WJ_CHAIN in the status global, as a function does for an exception; if it is the state of a caller after the
// call that returned that status, the code goes on to return in the way that a call does.
static int EmitSpill(Fn *cx, U32 cilOfs, tMD_MethodDef *callee, U32 recvDepth, U32 limit, int isMiss) {
	U32 i, off = 0, recvOfs = 0;
	if (cx->isEntry) {
		U32 site;
		FrameAll(cx, 1, 1, 1);
		for (i = 0; i < limit; i++) {
			U8 k = cx->st[i];
			if (i == recvDepth) recvOfs = off;
			if (k == T_VT) {
				Ref d = RefMem(1, off);
				if (!RefCopy(cx, &d, &cx->stRef[i], cx->stTy[i], cx->stTy[i]->stackSize)) return 0;
				off += cx->stTy[i]->stackSize;
			} else if (k == T_PTR && cx->stRef[i].mode == R_SCALAR) {            // a pointer to a struct that was in locals: now it is in the frame
				if (!cx->stRef[i].hasFrame) FAIL("a pointer to a struct that is not in the frame, at a point where a guard can fail");
				LGET(1); LGET(0); I32Const(cx, cx->stRef[i].frameHome + cx->stRef[i].off); OP(0x6a); Mem(cx, 0x36, off);
				off += 4;
			} else {
				LGET(1); LGET(Slot(cx, i, k)); Mem(cx, StoreOp(k), off);
				off += SizeOfKind(k);
			}
		}
		site = AddDeoptSite(cx, cilOfs, off, callee, recvOfs);
		Return(cx, WJ_DEOPT + (I32)site);
		return 1;
	} else {
		tMD_MethodDef *m = cx->m;
		tWasmFrameDesc *d;
		U32 stackBytes = 0, plBytes = m->parameterStackSize + cx->localsBytes, imgOff, imgL, numReloc = 0, *reloc = NULL;
		for (i = 0; i < limit; i++) {
			if (cx->st[i] == T_PTR && !(cx->stRef[i].mode == R_SCALAR && cx->stRef[i].hasFrame)) FAIL("a pointer on the stack at a place in a called method where a guard can fail");   // (it might be into this function's own frame)
			stackBytes += cx->st[i] == T_VT ? cx->stTy[i]->stackSize : SizeOfKind(cx->st[i]);
		}
		imgOff = AllocScratch(cx, plBytes + stackBytes);
		imgL = NewLocal(cx, T_I32);
		LGET(cx->sbLocal); I32Const(cx, imgOff); OP(0x6a); LSET(imgL);
		for (i = 0; i < cx->nArgs; i++) {                                        // the parameters, and the locals, where the interpreter's frame has them
			U32 po = m->pParams[i].offset; int k = cx->argT[i];
			if (k == T_VT) { Ref dst = RefMem(imgL, po); if (!RefCopy(cx, &dst, &cx->argRef[i], cx->argTy[i], cx->argTy[i]->stackSize)) return 0; }
			else { LGET(imgL); LGET(cx->argL[i]); Mem(cx, StoreOp(k), po); }
		}
		for (i = 0; i < cx->nLocals; i++) {
			U32 lo = m->parameterStackSize + cx->locOff[i]; int k = cx->locT[i];
			if (k == T_VT) { Ref dst = RefMem(imgL, lo); if (!RefCopy(cx, &dst, &cx->locRef[i], cx->locTy[i], cx->locTy[i]->stackSize)) return 0; }
			else { LGET(imgL); LGET(cx->locL[i]); Mem(cx, StoreOp(k), lo); }
		}
		off = plBytes;
		for (i = 0; i < limit; i++) {
			U8 k = cx->st[i];
			if (i == recvDepth) recvOfs = off - plBytes;
			if (k == T_VT) {
				Ref dst = RefMem(imgL, off);
				if (!RefCopy(cx, &dst, &cx->stRef[i], cx->stTy[i], cx->stTy[i]->stackSize)) return 0;
				off += cx->stTy[i]->stackSize;
			} else if (k == T_PTR) {                                              // a pointer to a struct in locals: an address in the frame that is built
				reloc = (U32*)realloc(reloc, (numReloc + 1) * 2 * sizeof(U32));
				reloc[2 * numReloc] = off - plBytes; reloc[2 * numReloc + 1] = cx->stRef[i].frameHome + cx->stRef[i].off; numReloc++;
				LGET(imgL); I32Const(cx, 0); Mem(cx, 0x36, off);
				off += 4;
			} else {
				LGET(imgL); LGET(Slot(cx, i, k)); Mem(cx, StoreOp(k), off);
				off += SizeOfKind(k);
			}
		}
		d = (tWasmFrameDesc*)mallocForever(sizeof(tWasmFrameDesc));
		memset(d, 0, sizeof(*d));
		if (numReloc > 0) { d->reloc = (U32*)mallocForever(numReloc * 2 * sizeof(U32)); memcpy(d->reloc, reloc, numReloc * 2 * sizeof(U32)); d->numReloc = numReloc; }
		free(reloc);
		d->method = m; d->cilOfs = cilOfs; d->plBytes = plBytes; d->stackBytes = stackBytes; d->callee = callee; d->recvOfs = recvOfs;
		cx->descs = (tWasmFrameDesc**)realloc(cx->descs, (cx->nDescs + 1) * sizeof(*cx->descs));
		cx->descs[cx->nDescs++] = d;
		I32Const(cx, (I32)(uintptr_t)d); LGET(imgL); I32Const(cx, (I32)(uintptr_t)&WasmJIT_DeoptPush); OP(0x11); BU(&cx->body, SIG_NEWARR); OP(0x00); OP(0x1a);
		cx->canDeopt = 1;
		if (isMiss) Throw(cx, WJ_CHAIN);
		return 1;
	}
}

static int EmitVirtualCall(Fn *cx, Ins *in) {
	tMD_MethodDef *m = (tMD_MethodDef*)in->ref;
	int isIface = TYPE_ISINTERFACE(m->pParentType) != 0;
	Mod *mod = cx->mod;
	PredCtx pc;
	Pred use[MAXPRED];
	U32 sp = cx->sp, n, base, i, k, np = 0, tgtLocal, resSp = 0, resKind = 0;
	tMD_TypeDef *resTy = NULL; Ref resRef;
	U8 sSt[MAX_STACK]; tMD_TypeDef *sTy[MAX_STACK]; Ref *sRef;
	if (cx->inl.active) FAIL("virtual call in inlined code");
	FillTypesUsed(cx);
	n = m->numberOfParameters;
	if (sp < n) FAIL("stack underflow");
	base = sp - n;
	if (cx->st[base] != T_REF) FAIL("virtual call on something that is not an object");
	memset(&pc, 0, sizeof(pc)); pc.m = m; pc.decl = m->pParentType; pc.isIface = isIface;
	CLIFile_ForEachType(PredVisit, &pc);
	for (i = 0; i < nObserved; i++) {                              // and the types that failed the guards of an earlier version of this method
		if (observed[i].entry == mod->entryM && observed[i].callee == m) PredVisit(observed[i].type, &pc);
	}
	if (pc.overflow) FAIL("virtual call with too many possible targets");
	for (i = 0; i < (U32)pc.n; i++) {                              // the targets that can be called from here
		tMD_MethodDef *t = pc.p[i].target;
		Fn *prep;
		if (!CalleeOK(cx, t, 0)) continue;
		prep = PrepFn(mod, t);
		if (prep == NULL) continue;
		if (!CanInline(cx, prep) && GetCallee(cx, t, 0) == NULL) continue;
		use[np++] = pc.p[i];
	}
	if (np == 0) FAIL("no target of a virtual call that can be compiled");
	if (WJDebug()) fprintf(stderr, "wasm-jit:   speculating on %s: %u of %d target(s)\n", Sys_GetMethodDesc(m), np, pc.n);
	for (i = 1; i < n; i++) if (cx->st[base + i] == T_VT && cx->stRef[base + i].mode == R_SCALAR) { if (!MaterializeVT(cx, base + i, 3)) return 0; }
	LGET(Slot(cx, base, T_REF)); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b);        // a null receiver
	// what the receiver is: the target that its vtable names, or for an interface its type
	tgtLocal = NewLocal(cx, T_I32);
	LGET(Slot(cx, base, T_REF)); I32Const(cx, -(I32)(sizeof(tHeapEntry) - offsetof(tHeapEntry, pTypeDef))); OP(0x6a); Mem(cx, 0x28, 0);
	if (!isIface) { Mem(cx, 0x28, (U32)offsetof(tMD_TypeDef, pVTable)); Mem(cx, 0x28, m->vTableOfs * 4); }
	LSET(tgtLocal);
	sRef = (Ref*)malloc((sp + 1) * sizeof(Ref));
	for (i = 0; i < sp; i++) { sSt[i] = cx->st[i]; sTy[i] = cx->stTy[i]; sRef[i] = cx->stRef[i]; }
#define RESTORE_STACK() do { cx->sp = sp; for (i = 0; i < sp; i++) { cx->st[i] = sSt[i]; cx->stTy[i] = sTy[i]; cx->stRef[i] = sRef[i]; } } while (0)
	for (k = 0; k < np; k++) {                                    // if it is this one: call it
		Ins fi;
		RESTORE_STACK();
		LGET(tgtLocal); I32Const(cx, isIface ? (I32)(uintptr_t)use[k].type : (I32)(uintptr_t)use[k].target); OP(0x46); OP(0x04); OP(0x40);
		memset(&fi, 0, sizeof(fi)); fi.op = 0x28; fi.ref = use[k].target; fi.off = in->off;
		if (!EmitCall(cx, &fi)) { free(sRef); return 0; }
		if (cx->sp > base && cx->st[base] == T_VT && cx->stRef[base].mode == R_SCALAR) { if (!MaterializeVT(cx, base, 0)) { free(sRef); return 0; } }
		if (k == 0) { resSp = cx->sp; resKind = cx->sp > base ? cx->st[base] : 0; resTy = cx->sp > base ? cx->stTy[base] : NULL; memset(&resRef, 0, sizeof(resRef)); if (cx->sp > base) resRef = cx->stRef[base]; }
		else if (cx->sp != resSp || (cx->sp > base && cx->st[base] != resKind)) { free(sRef); FAIL("the targets of a virtual call leave different results"); }
		OP(0x05);                                                 // else
	}
	RESTORE_STACK();                                              // none of them: the interpreter does it
	if (!EmitSpill(cx, in->off, m, base, sp, 1)) { free(sRef); return 0; }
	for (k = 0; k < np; k++) OP(0x0b);
#undef RESTORE_STACK
	cx->sp = resSp;                                               // (the stack as a call leaves it: the result, if any, where the receiver was)
	if (resSp > base) { cx->st[base] = (U8)resKind; cx->stTy[base] = resTy; cx->stRef[base] = resRef; }
	free(sRef);
	return 1;
}

// call / callvirt of a method (not virtual) and newobj, inlined if the callee is short and straight-line, else a call of the function that was
// compiled for it. The arguments of a call are the wasm values (a struct: the address of its temporary, which the callee may then use as its own
// copy); a struct result is written through an address the caller passes. All callees get the scratch area that follows the caller's.
static int EmitCall(Fn *cx, Ins *in) {
	tMD_MethodDef *c = (tMD_MethodDef*)in->ref;
	int isNew = in->op == 0x73, isVirt = in->op == 0x6f, inl;
	U32 sp = cx->sp, n, base, i, resOff = 0, newOff = 0, first = isNew ? 1 : 0, objLocal = 0, nBack = 0;
	struct { Ref sym; U32 depth; tMD_TypeDef *type; } back[8];
	tMD_TypeDef *newTy = NULL;
	Fn *prep, *callee = NULL, *sig;
	if (isVirt && (METHOD_ISVIRTUAL(c) || TYPE_ISINTERFACE(c->pParentType))) return EmitVirtualCall(cx, in);
	if (!CalleeOK(cx, c, isVirt)) return 0;
	prep = PrepFn(cx->mod, c);
	if (prep == NULL) { cx->why = cx->mod->why; return 0; }
	inl = CanInline(cx, prep);
	if (inl && isNew && c->pParentType->isValueType && LeavesOf(c->pParentType) == NULL) inl = 0;
	if (!inl) { callee = GetCallee(cx, c, isVirt); if (callee == NULL) return 0; }
	if (callee != NULL && callee->canDeopt) {            // (its frame may be given to the interpreter, and with it this one: that has to be possible)
		if (cx->inl.active) FAIL("call of a method that can deoptimize, in inlined code");
		if (isNew) FAIL("constructor that can deoptimize");
	}
	sig = inl ? prep : callee;
	n = sig->nArgs - first;
	POPN(n);
	base = sp - n;
	if (isNew) {
		newTy = c->pParentType;
		if (sig->argT[0] != (newTy->isValueType ? T_PTR : T_REF)) FAIL("constructor with an unexpected this");
	}
	for (i = 0; i < n; i++) {
		U8 want = sig->argT[i + first], have = cx->st[base + i];
		if (want != have || (want == T_VT && sig->argTy[i + first] != cx->stTy[base + i])) FAIL("argument of the wrong type");
	}
	if (n > 0 && isVirt && sig->argT[0] == T_REF) {              // callvirt on an object: a null `this` is a NullReferenceException
		LGET(Slot(cx, base, T_REF)); OP(0x45); OP(0x04); OP(0x40); Throw(cx, WJ_NULLREF); OP(0x0b);
	}
	if (inl) {
		if (isNew && !newTy->isValueType) {                      // an object: allocated (zeroed) first, then its constructor runs
			objLocal = NewLocal(cx, T_I32);
			I32Const(cx, (I32)(uintptr_t)newTy); I32Const(cx, (I32)(uintptr_t)&WasmJIT_NewObj); OP(0x11); BU(&cx->body, SIG_NEWOBJ); OP(0x00);
			LSET(objLocal);
		}
		return EmitInline(cx, prep, isNew, newTy, objLocal, base);
	}
	// A call takes addresses. A struct value held in locals is written to a temporary; a pointer to a struct that is held in locals (`this`) is
	// pointed at a temporary that is filled from it before the call and copied back after (the callee may change it).
	for (i = 0; i < n; i++) {
		U8 want = callee->argT[i + first];
		if (callee->canDeopt && want == T_PTR) FAIL("pointer to a struct passed to a method that can deoptimize");     // (what it does through it would not get back)
		if (want == T_VT && cx->stRef[base + i].mode == R_SCALAR) { if (!MaterializeVT(cx, base + i, 3)) return 0; }
		else if (want == T_PTR && cx->stRef[base + i].mode == R_SCALAR) {
			tMD_TypeDef *pt = callee->m->pParentType;
			U32 toff = TempOff(cx, base + i, pt, 4);
			Ref tmp;
			if (cx->failed) return 0;
			if (nBack >= 8) FAIL("too many struct pointers passed to one call");
			back[nBack].sym = cx->stRef[base + i]; back[nBack].depth = base + i; back[nBack].type = pt; nBack++;
			LGET(cx->sbLocal); I32Const(cx, toff); OP(0x6a); LSET(Slot(cx, base + i, T_PTR));
			tmp = RefMem(Slot(cx, base + i, T_PTR), 0);
			if (!RefCopy(cx, &tmp, &back[nBack - 1].sym, pt, TySize(pt))) return 0;
			cx->stRef[base + i] = tmp;
		}
	}
	if (isNew && newTy->isValueType) { Addr a; newOff = TempOff(cx, base, newTy, 2); a.local = cx->sbLocal; a.off = newOff; ZeroBytes(cx, a, TySize(newTy)); }
	if (isNew && !newTy->isValueType) {                  // an object: allocated (zeroed) first, then its constructor runs
		objLocal = NewLocal(cx, T_I32);
		I32Const(cx, (I32)(uintptr_t)newTy); I32Const(cx, (I32)(uintptr_t)&WasmJIT_NewObj); OP(0x11); BU(&cx->body, SIG_NEWOBJ); OP(0x00);
		LSET(objLocal);
	}
	if (callee->hasRet && callee->retKind == T_VT) resOff = TempOff(cx, base, callee->retTy, 1);
	if (cx->failed) return 0;
	LGET(cx->sbLocal); FrameSizeHole(cx); OP(0x6a);                                  // the callee's scratch area
	if (callee->hasRet && callee->retKind == T_VT) { LGET(cx->sbLocal); I32Const(cx, resOff); OP(0x6a); }
	if (isNew) { if (newTy->isValueType) { LGET(cx->sbLocal); I32Const(cx, newOff); OP(0x6a); } else LGET(objLocal); }
	for (i = 0; i < n; i++) LGET(Slot(cx, base + i, cx->st[base + i]));
	OP(0x10); BU(&cx->body, callee->index);
	cx->sp = base;
	if (isNew && !newTy->isValueType) {
		LGET(objLocal); LSET(Slot(cx, base, T_REF));
		cx->st[base] = T_REF; cx->stTy[base] = NULL; cx->sp++;
	} else if (isNew) {
		LGET(cx->sbLocal); I32Const(cx, newOff); OP(0x6a); LSET(Slot(cx, base, T_VT));
		cx->st[base] = T_VT; cx->stTy[base] = newTy; cx->stRef[base] = RefMem(Slot(cx, base, T_VT), 0); cx->sp++;
	} else if (callee->hasRet) {
		if (callee->retKind == T_VT) {
			LGET(cx->sbLocal); I32Const(cx, resOff); OP(0x6a); LSET(Slot(cx, base, T_VT));
			cx->st[base] = T_VT; cx->stTy[base] = callee->retTy; cx->stRef[base] = RefMem(Slot(cx, base, T_VT), 0);
		} else {
			LSET(Slot(cx, base, (int)callee->retKind));
			cx->st[base] = (U8)callee->retKind; cx->stTy[base] = NULL;
		}
		cx->sp++;
	}
	for (i = 0; i < nBack; i++) {                                                    // what the callee did to a struct that we held in locals
		Ref tmp = RefMem(Slot(cx, back[i].depth, T_PTR), 0);
		if (!RefCopy(cx, &back[i].sym, &tmp, back[i].type, TySize(back[i].type))) return 0;
	}
	if (callee->canDeopt) {                                                         // did it hand its frame over? then so does this one, at this call
		I32 idx = cx->insAt[in->off];                      // (the instruction: `in` may be the one made for a guarded target of a virtual call)
		if (idx < 0 || (U32)idx + 1 >= cx->n) FAIL("call that can deoptimize at the end of the code");
		OP(0x23); BU(&cx->body, 0); I32Const(cx, WJ_CHAIN); OP(0x46); OP(0x04); OP(0x40);
		if (!EmitSpill(cx, cx->ins[idx + 1].off, NULL, 0, base, 0)) return 0;
		OP(0x0b);
		cx->canDeopt = 1; cx->canThrow = 1;
	}
	if (callee->canThrow) {                                                         // did it raise one?
		OP(0x23); BU(&cx->body, 0); OP(0x04); OP(0x40);
		if (cx->isEntry) { OP(0x23); BU(&cx->body, 0); } else PushZeroRet(cx);
		OP(0x0f); OP(0x0b);
		cx->canThrow = 1;
	}
	{
		U32 j;
		for (j = 0; j < cx->nCallees && cx->callees[j] != callee; j++) { }
		if (j == cx->nCallees) { cx->callees = (Fn**)realloc(cx->callees, (cx->nCallees + 1) * sizeof(Fn*)); cx->callees[cx->nCallees++] = callee; }
	}
	return 1;
}

// the module: type, import (the memory), function, global (the status), export, code
static void Assemble(Mod *mod, Buf *out) {
	Buf sec = {0}, code = {0}, sigs[MAX_FNS + 2];
	U32 nSigs = 0, sigOf[MAX_FNS], i, j;
	memset(sigs, 0, sizeof(sigs));
	// type 0: (i32) -> i32 and type 1: (i32, i32) -> i32, for call_indirect of the allocation functions
	BPut(&sigs[0], 0x60); BPut(&sigs[0], 1); BPut(&sigs[0], 0x7f); BPut(&sigs[0], 1); BPut(&sigs[0], 0x7f);
	BPut(&sigs[1], 0x60); BPut(&sigs[1], 2); BPut(&sigs[1], 0x7f); BPut(&sigs[1], 0x7f); BPut(&sigs[1], 1); BPut(&sigs[1], 0x7f);
	nSigs = 2;
	for (i = 0; i < mod->nFns; i++) {
		Fn *f = mod->fns[i];
		Buf sg = {0};
		BPut(&sg, 0x60);
		if (f->isEntry) { BPut(&sg, 4); BPut(&sg, 0x7f); BPut(&sg, 0x7f); BPut(&sg, 0x7f); BPut(&sg, 0x7f); BPut(&sg, 1); BPut(&sg, 0x7f); }
		else {
			int sret = f->hasRet && f->retKind == T_VT;
			BPut(&sg, 1 + sret + f->nArgs); BPut(&sg, 0x7f); if (sret) BPut(&sg, 0x7f);
			for (j = 0; j < f->nArgs; j++) BPut(&sg, wt[f->argT[j]]);
			if (f->hasRet && !sret) { BPut(&sg, 1); BPut(&sg, wt[f->retKind]); } else BPut(&sg, 0);
		}
		for (j = 0; j < nSigs; j++) if (sigs[j].n == sg.n && memcmp(sigs[j].p, sg.p, sg.n) == 0) break;
		if (j == nSigs) sigs[nSigs++] = sg; else free(sg.p);
		sigOf[i] = j;
	}
	BMem(out, "\0asm\1\0\0\0", 8);
	BU(&sec, nSigs); for (i = 0; i < nSigs; i++) BMem(&sec, sigs[i].p, sigs[i].n);
	BSection(out, 1, &sec); sec.n = 0;
	BPut(&sec, 2);
	BPut(&sec, 3); BMem(&sec, "env", 3); BPut(&sec, 6); BMem(&sec, "memory", 6); BPut(&sec, 2); BPut(&sec, 0); BPut(&sec, 0);
	BPut(&sec, 3); BMem(&sec, "env", 3); BPut(&sec, 5); BMem(&sec, "table", 5); BPut(&sec, 1); BPut(&sec, 0x70); BPut(&sec, 0); BPut(&sec, 0);   // the indirect function table
	BSection(out, 2, &sec); sec.n = 0;
	BU(&sec, mod->nFns); for (i = 0; i < mod->nFns; i++) BU(&sec, sigOf[i]);
	BSection(out, 3, &sec); sec.n = 0;
	BPut(&sec, 1); BPut(&sec, 0x7f); BPut(&sec, 1); BPut(&sec, 0x41); BPut(&sec, 0); BPut(&sec, 0x0b);       // (global i32 (mut) = 0)
	BSection(out, 6, &sec); sec.n = 0;
	BPut(&sec, 1); BPut(&sec, 1); BPut(&sec, 'f'); BPut(&sec, 0); BPut(&sec, 0);                              // export "f" = function 0
	BSection(out, 7, &sec); sec.n = 0;
	BU(&code, mod->nFns);
	for (i = 0; i < mod->nFns; i++) {
		Fn *f = mod->fns[i];
		Buf fn = {0};
		if (f->isDispatcher) {
			// switch (entry) { case 0: return F0(...); case k: return Fk(...) }: the interpreter calls this one function, and gets back into the loop it left
			U32 cases = f->nVariantFns;
			BU(&fn, 0);
			for (j = 0; j < cases; j++) { BPut(&fn, 0x02); BPut(&fn, 0x40); }
			BPut(&fn, 0x20); BU(&fn, 2);
			BPut(&fn, 0x0e); BU(&fn, cases); for (j = 0; j < cases; j++) BU(&fn, j); BU(&fn, cases - 1);
			for (j = 0; j < cases; j++) {
				BPut(&fn, 0x0b);
				BPut(&fn, 0x20); BU(&fn, 0); BPut(&fn, 0x20); BU(&fn, 1); BPut(&fn, 0x20); BU(&fn, 2); BPut(&fn, 0x20); BU(&fn, 3);
				BPut(&fn, 0x10); BU(&fn, f->variantFn[j]); BPut(&fn, 0x0f);
			}
			BPut(&fn, 0x00); BPut(&fn, 0x0b);
		} else {
			BU(&fn, f->numWasmLocals);
			for (j = 0; j < f->numWasmLocals; j++) { BPut(&fn, 1); BPut(&fn, wt[f->localKinds[j]]); }
			BMem(&fn, f->body.p, f->body.n);
		}
		BU(&code, fn.n); BMem(&code, fn.p, fn.n);
		free(fn.p);
	}
	BSection(out, 10, &code);
	free(sec.p); free(code.p);
	for (i = 0; i < nSigs; i++) free(sigs[i].p);
}

U32 WasmJIT_Slice(void) {
	static U32 v = 0;
	if (v == 0) {
		v = getenv("DNA_WASM_JIT_SLICE") != NULL ? (U32)atoi(getenv("DNA_WASM_JIT_SLICE")) : WJ_SLICE;
		if (v == 0) v = 1;
	}
	return v;
}

int WasmJIT_Enabled(void) {
	static int v = -1;
	if (v < 0) v = getenv("DNA_NO_WASM_JIT") == NULL;
	return v;
}

U32 WasmJIT_Compile(tMD_MethodDef *m, const U8 *cil, U32 codeSize, const tParameter *pLocals, U32 numLocals, U32 origLocalsSize, tWasmResult *pResult) {
	static int debug = -1;
	Mod *mod;
	Fn *entry;
	U32 i, result = 0;
	Buf out = {0};
	int idx;

	memset(pResult, 0, sizeof(*pResult));
	if (!WasmJIT_Enabled() || codeSize > 60000) return 0;
	{
		// DNA_WASM_JIT_LIMIT=n: compile only the first n methods that qualify (to find which one breaks something, by bisection)
		static int limit = -2, count = 0;
		if (limit == -2) limit = getenv("DNA_WASM_JIT_LIMIT") != NULL ? atoi(getenv("DNA_WASM_JIT_LIMIT")) : -1;
		if (limit >= 0 && count >= limit) return 0;
		count++;
	}
	if (debug < 0) debug = getenv("DNA_WASM_JIT_DEBUG") != NULL;
	mod = (Mod*)calloc(1, sizeof(Mod));
	mod->entryM = m; mod->entryCil = cil; mod->entrySize = codeSize; mod->entryLocals = pLocals; mod->entryNumLocals = numLocals; mod->entryLocalsSize = origLocalsSize; mod->headerMaxStack = HeaderMaxStack(m);
	// (the scratch area follows the frame's parameters and locals)
	entry = CompileFn(mod, m, 1, cil, codeSize, pLocals, numLocals, m->parameterStackSize + origLocalsSize);
	if (entry == NULL) goto done;
	Assemble(mod, &out);
	if (getenv("DNA_WASM_JIT_DUMP") != NULL) {
		char name[256];
		FILE *f;
		snprintf(name, sizeof(name), "%s/m%08x.wasm", getenv("DNA_WASM_JIT_DUMP"), (unsigned)(uintptr_t)m);
		f = fopen(name, "wb");
		if (f != NULL) { fwrite(out.p, 1, out.n, f); fclose(f); }
	}
	idx = dna_emit_wasm(out.p, out.n);
	if (idx <= 0) { mod->why = "the host refused the module"; goto done; }
	result = (U32)idx;
	pResult->extraFrame = entry->need;
	for (i = 0; i < mod->nFns; i++) {
		// A method that was compiled to be called can give its frame to the interpreter too (EmitSpill): its interpreter's version, with the places where it goes on
		Fn *f = mod->fns[i];
		U32 j, *keep, *cilToOp = NULL;
		tJITted *interp = NULL;
		if (f->isEntry || f->nDescs == 0) continue;
		keep = (U32*)malloc(f->nDescs * sizeof(U32));
		for (j = 0; j < f->nDescs; j++) keep[j] = f->descs[j]->cilOfs;
		JIT_BuildInterpreterVersion(f->m, (U8*)f->cil, f->size, f->locParams, HeaderMaxStack(f->m), f->localsBytes, keep, f->nDescs, NULL, 0, &interp, &cilToOp);
		for (j = 0; j < f->nDescs; j++) { f->descs[j]->interp = interp; f->descs[j]->ip = cilToOp[f->descs[j]->cilOfs]; }
		free(keep); free(cilToOp);
	}
	if (mod->nSites > 0) {
		// The interpreter's version, for a frame that has to leave the compiled code at a place where a guard can fail. It keeps its fused ops except that
		// those places start an op, and it has an op at each header that the compiled code can be entered at, where the frame goes back.
		U32 i, nKeep = mod->nSites, nOsr = 0, *keep = (U32*)malloc(mod->nSites * sizeof(U32)), *osrOfs = NULL;
		tWasmDeopt *d = (tWasmDeopt*)mallocForever(sizeof(tWasmDeopt));
		for (i = 0; i < nKeep; i++) keep[i] = mod->sites[i].ip;
		if (entry->isDispatcher && entry->nLoopIds > 0) {
			osrOfs = (U32*)malloc(entry->nLoopIds * sizeof(U32));
			d->osr = (tWasmOsr*)mallocForever(entry->nLoopIds * sizeof(tWasmOsr));
			for (i = 1; i <= entry->nLoopIds; i++) {
				U32 ofs = entry->ins[entry->blockStart[entry->loopHeaders[i - 1]]].off;
				osrOfs[nOsr] = ofs; d->osr[nOsr].cilOfs = ofs; d->osr[nOsr].entry = i; nOsr++;
			}
		}
		d->numOsr = nOsr;
		JIT_BuildInterpreterVersion(m, (U8*)cil, codeSize, (tParameter*)pLocals, mod->headerMaxStack, origLocalsSize, keep, nKeep, osrOfs, nOsr, &mod->interp, &mod->cilToOp);
		free(keep); free(osrOfs);
		d->interp = mod->interp; d->numSites = mod->nSites;
		d->sites = (tWasmDeoptSite*)mallocForever(mod->nSites * sizeof(tWasmDeoptSite));
		for (i = 0; i < mod->nSites; i++) { d->sites[i] = mod->sites[i]; d->sites[i].ip = mod->cilToOp[mod->sites[i].ip]; }
		d->cil = cil; d->codeSize = codeSize; d->numLocals = numLocals; d->origLocalsSize = origLocalsSize; d->recompiles = 0;
		d->locals = NULL;
		if (numLocals > 0) { d->locals = (tParameter*)mallocForever(numLocals * sizeof(tParameter)); memcpy(d->locals, pLocals, numLocals * sizeof(tParameter)); }
		pResult->deopt = d;
		pResult->maxStack = (mod->maxStackBytes > mod->interp->maxStack ? mod->maxStackBytes : mod->interp->maxStack) + 16;
	}
done:
	if (debug && (result != 0 || !(mod->why != NULL && strcmp(mod->why, "too small to be worth it") == 0 && 0))) {
		if (result) fprintf(stderr, "wasm-jit: compiled %s (%u function%s, %u bytes of scratch)\n", Sys_GetMethodDesc(m), mod->nFns, mod->nFns == 1 ? "" : "s", pResult->extraFrame);
		else fprintf(stderr, "wasm-jit: skipped  %s: %s\n", Sys_GetMethodDesc(m), mod->why ? mod->why : "?");
	}
	for (i = 0; i < mod->nFns; i++) FreeFn(mod->fns[i]);
	for (i = 0; i < mod->nPreps; i++) FreeFn(mod->preps[i]);
	free(mod->sites); free(mod->cilToOp);
	free(mod); free(out.p);
	return result;
}

#endif

// Whether the compiled version of a method can be entered at the loop whose header is at this CIL offset (see JIT_WASM_OSR), and with what entry value.
int WasmJIT_OsrEntry(const tJITted *pCompiled, U32 cilOfs, U32 *pEntry) {
	const tWasmDeopt *d = (const tWasmDeopt*)pCompiled->pWasmDeopt;
	U32 i;
	if (d == NULL) return 0;
	for (i = 0; i < d->numOsr; i++) {
		if (d->osr[i].cilOfs == cilOfs) { *pEntry = d->osr[i].entry; return 1; }
	}
	return 0;
}
