// The register pass: see VStack.h and tools/gen_vstencils.py.
//
// The plain stencils of a block each do what one instruction does to the evaluation stack in memory, so  x = a + b  is  ldl a; ldl b; iadd; stl x
// and four trips through memory. This pass walks that list with a *virtual* evaluation stack, whose entries say where a value is without it
// being anywhere yet: in a local (a frame offset), a constant, or the one register that is used as a cache (eax / rax, or xmm0). A load pushes
// such an entry and emits nothing; an operator takes its operands' places as the places to read them from, and emits a single three-address
// stencil that leaves the result in the register, or, if the next stencil stores it, in the local it is stored to.
//
// Anything the pass does not understand takes its operands from the evaluation stack in memory, so the virtual stack is first written there
// (flushed), in order, and the stencil is copied as it is. The same is done at every place that something can jump or be entered to,
// and at the end of the block, so the rest of the compiler never sees a value that is not in memory: branches, entries, islands, exits,
// inlined recipes and null and index checks all work as they did.
//
// Two rules keep it right. A value that is only a description of a local is read late, so before a store to that local (or to anything that may
// overlap it) every entry that describes it is flushed. And only one entry at a time is in the register, so a result is made only
// when no other entry is, or after that one has been flushed.

#include "Compat.h"
#include "Sys.h"
#include "VStack.h"
#include "NativeBlocks.h"

#if NATIVE_BLOCKS

#include "VStencils.gen.h"

#define NONE 0xffffffffu
#define VS_MAX 24

typedef struct {
	int loc;        // VK_L, VK_C, VK_R, VK_K (a constant of the pool) or VK_M (the top of the stack in memory: only made up for an operand)
	int sz;         // 4 or 8
	int cls;        // for VK_R: 0 integer (eax / rax), 1 floating point (xmm0)
	U32 val;        // a frame offset, the constant, or the index in the pool
} tVE;

typedef struct {
	tVE e[VS_MAX];
	int sp;
	tStencilList out;
	U32 n;
	U64 *pool;      // the 64-bit constants (VK_K)
	U32 numPool, capPool;
} tVS;

static U32 PoolIndex(tVS *s, U64 v) {
	U32 i;
	for (i = 0; i < s->numPool; i++) {
		if (s->pool[i] == v) { return i; }
	}
	if (s->numPool == s->capPool) {
		s->capPool = s->capPool ? s->capPool * 2 : 8;
		s->pool = (U64*)realloc(s->pool, s->capPool * sizeof(U64));
	}
	s->pool[s->numPool] = v;
	return s->numPool++;
}

static void CopyIn(tVS *s, const tStencilList *in, U32 i) {
	s->out.ids[s->n] = in->ids[i];
	s->out.hole0[s->n] = in->hole0[i]; s->out.hole1[s->n] = in->hole1[i]; s->out.hole2[s->n] = in->hole2[i];
	s->out.srcInstr[s->n] = in->srcInstr[i];
	s->out.iwords[s->n] = in->iwords[i]; s->out.ilen[s->n] = in->ilen[i]; s->out.ientry[s->n] = in->ientry[i];
	s->out.endJump[s->n] = in->endJump[i];
	s->out.tgt[s->n] = in->tgt[i];
	s->n++;
}

static void Out(tVS *s, int id, U32 a, U32 b, U32 c) {
	s->out.ids[s->n] = (unsigned)id;
	s->out.hole0[s->n] = a; s->out.hole1[s->n] = b; s->out.hole2[s->n] = c;
	s->out.srcInstr[s->n] = NONE;
	s->out.iwords[s->n] = NULL; s->out.ilen[s->n] = 0; s->out.ientry[s->n] = 0;
	s->out.endJump[s->n] = 0;
	s->out.tgt[s->n] = -1;
	s->n++;
}

// which of i l f d a value is, for the tables of stencils that store it and push it
static int ClassIndex(const tVE *e) {
	if (e->loc == VK_R && e->cls == 1) { return e->sz == 4 ? 2 : 3; }
	return e->sz == 4 ? 0 : 1;
}

// write the entries 0..k to the evaluation stack in memory, in order, and drop them
static void FlushDownTo(tVS *s, int k) {
	int i;
	for (i = 0; i <= k; i++) {
		const tVE *e = &s->e[i];
		int id = vst_push[ClassIndex(e)][e->loc];
		if (e->loc == VK_R) { Out(s, id, 0, 0, 0); } else if (e->loc == VK_K) { Out(s, id, 0, 0, e->val); } else { Out(s, id, e->val, 0, 0); }
	}
	for (i = k + 1; i < s->sp; i++) {
		s->e[i - k - 1] = s->e[i];
	}
	s->sp -= k + 1;
}

static void FlushAll(tVS *s) {
	if (s->sp > 0) { FlushDownTo(s, s->sp - 1); }
}

static int Overlaps(const tVE *e, U32 ofs, int sz) {
	return e->loc == VK_L && e->val < ofs + (U32)sz && ofs < e->val + (U32)e->sz;
}

// before a write to the frame at [ofs, ofs+sz): flush the entries (below the top `skip`) that describe what is there
static void FlushOverlapping(tVS *s, U32 ofs, int sz, int skip) {
	int i, last = -1;
	for (i = 0; i < s->sp - skip; i++) {
		if (Overlaps(&s->e[i], ofs, sz)) { last = i; }
	}
	if (last >= 0) { FlushDownTo(s, last); }
}

// only one entry may be in the register: if one is below the top `skip` entries, flush down to it
static void SpillRegister(tVS *s, int skip) {
	int i, last = -1;
	for (i = 0; i < s->sp - skip; i++) {
		if (s->e[i].loc == VK_R) { last = i; }
	}
	if (last >= 0) { FlushDownTo(s, last); }
}

static void Push(tVS *s, int loc, int sz, int cls, U32 val) {
	tVE *e;
	if (s->sp == VS_MAX) { FlushAll(s); }
	e = &s->e[s->sp++];
	e->loc = loc; e->sz = sz; e->cls = cls; e->val = val;
}

// the operands of an instruction: the values of the entries that are not in the register, in order, then the destination
static int HasValue(const tVE *e) {
	return e != NULL && (e->loc == VK_L || e->loc == VK_C);
}

// Emit a stencil for operands a and b (either may be NULL): the values of a and b that have one (a local, a constant), then `extra` if
// given, are its value holes; a constant of the pool takes the third (HOLE5 reads it)
static void OutOperands(tVS *s, int id, const tVE *a, const tVE *b, int hasExtra, U32 extra) {
	U32 v[4] = { 0, 0, 0, 0 };
	int n = 0, k = -1;
	if (HasValue(a)) { v[n++] = a->val; }
	if (HasValue(b)) { v[n++] = b->val; }
	if (hasExtra) { v[n++] = extra; }
	if (a != NULL && a->loc == VK_K) { k = (int)a->val; }
	if (b != NULL && b->loc == VK_K) { k = (int)b->val; }
	Out(s, id, v[0], v[1], k >= 0 ? (U32)k : v[2]);
}

typedef struct {
	int kind;       // what the stencil is
	int op;         // which, within its table
} tSem;
enum { S_NONE = 0, S_LDL, S_LDP, S_LDC, S_LDC8, S_STL, S_STP, S_I32, S_I64, S_F32, S_F64, S_ICC, S_LCC, S_FCC, S_DCC, S_JT, S_CVT, S_DUP4, S_DUP8 };

static void Classify(unsigned id, tSem *m) {
	m->kind = S_NONE; m->op = 0;
	switch (id) {
	case ST_LDL: m->kind = S_LDL; return;
	case ST_LDP: m->kind = S_LDP; return;
	case ST_LDC: m->kind = S_LDC; return;
	case ST_LDC8LO: m->kind = S_LDC8; return;
	case ST_STL: m->kind = S_STL; return;
	case ST_STP: m->kind = S_STP; return;
	case ST_DUP4: m->kind = S_DUP4; return;
	case ST_DUP8: m->kind = S_DUP8; return;
	case ST_IADD: m->kind = S_I32; m->op = 0; return;
	case ST_ISUB: m->kind = S_I32; m->op = 1; return;
	case ST_IMUL: m->kind = S_I32; m->op = 2; return;
	case ST_IAND: m->kind = S_I32; m->op = 3; return;
	case ST_IOR: m->kind = S_I32; m->op = 4; return;
	case ST_IXOR: m->kind = S_I32; m->op = 5; return;
	case ST_ISHL: m->kind = S_I32; m->op = 6; return;
	case ST_ISHR: m->kind = S_I32; m->op = 7; return;
	case ST_ISHRUN: m->kind = S_I32; m->op = 8; return;
	case ST_LADD: m->kind = S_I64; m->op = 0; return;
	case ST_LSUB: m->kind = S_I64; m->op = 1; return;
	case ST_LMUL: m->kind = S_I64; m->op = 2; return;
	case ST_LAND: m->kind = S_I64; m->op = 3; return;
	case ST_LOR: m->kind = S_I64; m->op = 4; return;
	case ST_LXOR: m->kind = S_I64; m->op = 5; return;
	case ST_FADD: m->kind = S_F32; m->op = 0; return;
	case ST_FSUB: m->kind = S_F32; m->op = 1; return;
	case ST_FMUL: m->kind = S_F32; m->op = 2; return;
	case ST_FDIV: m->kind = S_F32; m->op = 3; return;
	case ST_DADD: m->kind = S_F64; m->op = 0; return;
	case ST_DSUB: m->kind = S_F64; m->op = 1; return;
	case ST_DMUL: m->kind = S_F64; m->op = 2; return;
	case ST_DDIV: m->kind = S_F64; m->op = 3; return;
	case ST_JEQ: m->kind = S_ICC; m->op = 0; return;
	case ST_JNE: m->kind = S_ICC; m->op = 1; return;
	case ST_JLT: m->kind = S_ICC; m->op = 2; return;
	case ST_JLE: m->kind = S_ICC; m->op = 3; return;
	case ST_JGT: m->kind = S_ICC; m->op = 4; return;
	case ST_JGE: m->kind = S_ICC; m->op = 5; return;
	case ST_LBEQ: m->kind = S_LCC; m->op = 0; return;
	case ST_LBNE: m->kind = S_LCC; m->op = 1; return;
	case ST_LBLT: m->kind = S_LCC; m->op = 2; return;
	case ST_LBLE: m->kind = S_LCC; m->op = 3; return;
	case ST_LBGT: m->kind = S_LCC; m->op = 4; return;
	case ST_LBGE: m->kind = S_LCC; m->op = 5; return;
	case ST_JFEQ: m->kind = S_FCC; m->op = 0; return;
	case ST_JFNE: m->kind = S_FCC; m->op = 1; return;
	case ST_JFLT: m->kind = S_FCC; m->op = 2; return;
	case ST_JFLE: m->kind = S_FCC; m->op = 3; return;
	case ST_JFGT: m->kind = S_FCC; m->op = 4; return;
	case ST_JFGE: m->kind = S_FCC; m->op = 5; return;
	case ST_JFLT_UN: m->kind = S_FCC; m->op = 6; return;
	case ST_JFLE_UN: m->kind = S_FCC; m->op = 7; return;
	case ST_JFGT_UN: m->kind = S_FCC; m->op = 8; return;
	case ST_JFGE_UN: m->kind = S_FCC; m->op = 9; return;
	case ST_JDEQ: m->kind = S_DCC; m->op = 0; return;
	case ST_JDNE: m->kind = S_DCC; m->op = 1; return;
	case ST_JDLT: m->kind = S_DCC; m->op = 2; return;
	case ST_JDLE: m->kind = S_DCC; m->op = 3; return;
	case ST_JDGT: m->kind = S_DCC; m->op = 4; return;
	case ST_JDGE: m->kind = S_DCC; m->op = 5; return;
	case ST_JDLT_UN: m->kind = S_DCC; m->op = 6; return;
	case ST_JDLE_UN: m->kind = S_DCC; m->op = 7; return;
	case ST_JDGT_UN: m->kind = S_DCC; m->op = 8; return;
	case ST_JDGE_UN: m->kind = S_DCC; m->op = 9; return;
	case ST_JT: m->kind = S_JT; m->op = 0; return;
	case ST_JF: m->kind = S_JT; m->op = 1; return;
	case ST_JT8: m->kind = S_JT; m->op = 2; return;
	case ST_JF8: m->kind = S_JT; m->op = 3; return;
	case ST_CVTIL: m->kind = S_CVT; m->op = 0; return;
	case ST_CVTUL: m->kind = S_CVT; m->op = 1; return;
	case ST_CVTIF: m->kind = S_CVT; m->op = 2; return;
	case ST_CVTID: m->kind = S_CVT; m->op = 3; return;
	case ST_CVTFD: m->kind = S_CVT; m->op = 4; return;
	case ST_CVTDF: m->kind = S_CVT; m->op = 5; return;
	}
}

// An operator on the two entries on top, with `sz` bytes each (the second operand of a shift is 4); tab is its family of stencils.
// Returns 1 if it was done (and consumed the following store, if *pConsumed is set), else 0 with nothing emitted or changed.
static int Binary(tVS *s, const short (*tab)[5][5][3], int op, int sz, int cls, int szB, int canRmw, const tStencilList *in, U32 i, U32 n,
		const unsigned char *flushAt, int *pConsumed) {
	tVE a, b;
	int dst = 0, id, pops;
	int storeNext = 0;
	U32 d = 0;
	*pConsumed = 0;
	if (s->sp < 1) { return 0; }
	b = s->e[s->sp - 1];
	if (s->sp >= 2) {
		a = s->e[s->sp - 2]; pops = 2;
	} else {
		a.loc = VK_M; a.sz = sz; a.cls = 0; a.val = 0; pops = 1;      // the first operand is on the stack in memory
	}
	if (a.sz != sz || b.sz != szB || (a.loc == VK_R && a.cls != cls) || (b.loc == VK_R && b.cls != cls && !(szB == 4 && cls == 0 && b.cls == 0))) { return 0; }
	if (a.loc == VK_R && b.loc == VK_R) { return 0; }
	if (tab[op][a.loc][b.loc][0] < 0) { return 0; }
	// the store that follows, if the result is wanted in a local
	if (i + 1 < n && !flushAt[i + 1]) {
		tSem m;
		Classify(in->ids[i + 1], &m);
		if ((m.kind == S_STL && sz == 4) || (m.kind == S_STP && sz == 8)) { storeNext = 1; d = in->hole0[i + 1]; }
	}
	s->sp -= pops;              // (the operands are now ours)
	SpillRegister(s, 0);
	if (storeNext) {
		FlushOverlapping(s, d, sz, 0);
		if (canRmw && a.loc == VK_L && a.val == d && tab[op][a.loc][b.loc][2] >= 0) { dst = 2; }
		else if (tab[op][a.loc][b.loc][1] >= 0) { dst = 1; }
		else { storeNext = 0; }
	}
	id = tab[op][a.loc][b.loc][dst];
	OutOperands(s, id, &a, &b, dst == 1, d);
	if (dst == 0) {
		tVE *r;
		if (s->sp == VS_MAX) { FlushAll(s); }
		r = &s->e[s->sp++];
		r->loc = VK_R; r->sz = sz; r->cls = cls; r->val = 0;
	}
	*pConsumed = (dst != 0 && storeNext);
	return 1;
}

U32 VStack_Run(tStencilList *l, U32 n, const unsigned char *flushAt, U32 *remap, U64 **pPool, U32 *pNumPool) {
	static int disabled = -1;
	tVS s;
	U32 i, cap = n + 16, k;
	*pPool = NULL; *pNumPool = 0;
	if (disabled < 0) { disabled = (getenv("DNA_NO_VSTACK") != NULL); }
	if (disabled) {
		for (i = 0; i <= n; i++) { remap[i] = i; }
		return n;
	}
	s.sp = 0; s.n = 0; s.pool = NULL; s.numPool = 0; s.capPool = 0;
	s.out.ids = (unsigned*)malloc(cap * sizeof(unsigned));
	s.out.hole0 = (U32*)malloc(cap * sizeof(U32)); s.out.hole1 = (U32*)malloc(cap * sizeof(U32)); s.out.hole2 = (U32*)malloc(cap * sizeof(U32));
	s.out.srcInstr = (U32*)malloc(cap * sizeof(U32));
	s.out.iwords = (const tOpWord**)malloc(cap * sizeof(tOpWord*));
	s.out.ilen = (U32*)malloc(cap * sizeof(U32)); s.out.ientry = (U32*)malloc(cap * sizeof(U32));
	s.out.endJump = (char*)malloc(cap);
	s.out.tgt = (int*)malloc(cap * sizeof(int));

	for (i = 0; i < n; i++) {
		tSem m;
		int consumed = 0;
		if (flushAt[i]) { FlushAll(&s); }
		remap[i] = s.n;
		Classify(l->ids[i], &m);
		switch (m.kind) {
		case S_LDL: Push(&s, VK_L, 4, 0, l->hole0[i]); continue;
		case S_LDP: Push(&s, VK_L, 8, 0, l->hole0[i]); continue;
		case S_LDC: Push(&s, VK_C, 4, 0, l->hole0[i]); continue;
		case S_LDC8:
			if (i + 1 < n && l->ids[i + 1] == ST_LDC8HI && !flushAt[i + 1]) {
				U32 lo = l->hole0[i], hi = l->hole0[i + 1];
				U64 v = (U64)lo | ((U64)hi << 32);
				remap[i + 1] = s.n;
				i++;
				if (hi == ((lo & 0x80000000u) ? 0xffffffffu : 0u)) { Push(&s, VK_C, 8, 0, lo); }        // fits an immediate (sign-extended)
				else { Push(&s, VK_K, 8, 0, PoolIndex(&s, v)); }
				continue;
			}
			break;
		case S_DUP4:
		case S_DUP8:
			if (s.sp > 0 && s.e[s.sp - 1].loc != VK_R && s.e[s.sp - 1].sz == (m.kind == S_DUP4 ? 4 : 8)) {
				tVE top = s.e[s.sp - 1];
				Push(&s, top.loc, top.sz, top.cls, top.val);
				continue;
			}
			break;
		case S_STL:
		case S_STP: {
			int sz = (m.kind == S_STL) ? 4 : 8;
			if (s.sp > 0 && s.e[s.sp - 1].sz == sz) {
				tVE top = s.e[s.sp - 1];
				U32 dest = l->hole0[i];
				s.sp--;
				FlushOverlapping(&s, dest, sz, 0);
				if (top.loc == VK_R) {
					Out(&s, vst_st[ClassIndex(&top)][VK_R], dest, 0, 0);
				} else if (top.loc == VK_K) {
					Out(&s, vst_st[ClassIndex(&top)][VK_K], dest, 0, top.val);
				} else if (vst_st[ClassIndex(&top)][top.loc] >= 0) {
					Out(&s, vst_st[ClassIndex(&top)][top.loc], top.val, dest, 0);
				} else {
					s.sp++; s.e[s.sp - 1] = top;                // (no stencil for that: the plain one, with the value on the stack)
					FlushAll(&s);
					CopyIn(&s, l, i);
				}
				continue;
			}
			break;
		}
		case S_I32:
			if (Binary(&s, vst_i32, m.op, 4, 0, 4, m.op <= 5 && m.op != 2, l, i, n, flushAt, &consumed)) {
				if (consumed) { remap[i + 1] = s.n; i++; }
				continue;
			}
			break;
		case S_I64:
			if (Binary(&s, vst_i64, m.op, 8, 0, 8, m.op != 2, l, i, n, flushAt, &consumed)) {
				if (consumed) { remap[i + 1] = s.n; i++; }
				continue;
			}
			break;
		case S_F32:
			if (Binary(&s, vst_f32, m.op, 4, 1, 4, 0, l, i, n, flushAt, &consumed)) {
				if (consumed) { remap[i + 1] = s.n; i++; }
				continue;
			}
			break;
		case S_F64:
			if (Binary(&s, vst_f64, m.op, 8, 1, 8, 0, l, i, n, flushAt, &consumed)) {
				if (consumed) { remap[i + 1] = s.n; i++; }
				continue;
			}
			break;
		case S_ICC:
		case S_LCC:
		case S_FCC:
		case S_DCC: {
			int sz = (m.kind == S_ICC || m.kind == S_FCC) ? 4 : 8;
			int cls = (m.kind == S_ICC || m.kind == S_LCC) ? 0 : 1;
			int id = -1;
			if (s.sp >= 1) {
				tVE a, b = s.e[s.sp - 1];
				int pops = 2;
				if (s.sp >= 2) { a = s.e[s.sp - 2]; } else { a.loc = VK_M; a.sz = sz; a.cls = 0; a.val = 0; pops = 1; }
				if (a.sz == sz && b.sz == sz && !(a.loc == VK_R && b.loc == VK_R) && (a.loc != VK_R || a.cls == cls) && (b.loc != VK_R || b.cls == cls)) {
					id = (m.kind == S_ICC) ? vst_icc[m.op][a.loc][b.loc] : (m.kind == S_LCC) ? vst_lcc[m.op][a.loc][b.loc] :
						(m.kind == S_FCC) ? vst_fcc[m.op][a.loc][b.loc] : vst_dcc[m.op][a.loc][b.loc];
					if (id >= 0 && !(a.loc == VK_M && s.sp - 1 > 0)) {
						s.sp -= pops;
						FlushAll(&s);                           // (what is below goes to the stack before the branch: both ways see it there)
						OutOperands(&s, id, &a, &b, 0, 0);
						s.n--;                                  // (OutOperands made the stencil; the rest of its entry is the branch's)
						s.out.srcInstr[s.n] = l->srcInstr[i];
						s.out.iwords[s.n] = l->iwords[i]; s.out.ilen[s.n] = l->ilen[i]; s.out.ientry[s.n] = l->ientry[i];
						s.out.endJump[s.n] = l->endJump[i];
						s.out.tgt[s.n] = l->tgt[i];
						s.n++;
						continue;
					}
				}
			}
			break;
		}
		case S_JT: {
			int sz = (m.op >= 2) ? 8 : 4;
			if (s.sp >= 1) {
				tVE a = s.e[s.sp - 1];
				if (a.sz == sz && a.loc != VK_C && (a.loc != VK_R || a.cls == 0) && vst_jt[m.op][a.loc] >= 0) {
					s.sp--;
					FlushAll(&s);
					s.out.ids[s.n] = (unsigned)vst_jt[m.op][a.loc];
					s.out.hole0[s.n] = HasValue(&a) ? a.val : 0; s.out.hole1[s.n] = 0; s.out.hole2[s.n] = 0;
					s.out.srcInstr[s.n] = l->srcInstr[i];
					s.out.iwords[s.n] = l->iwords[i]; s.out.ilen[s.n] = l->ilen[i]; s.out.ientry[s.n] = l->ientry[i];
					s.out.endJump[s.n] = l->endJump[i];
					s.out.tgt[s.n] = l->tgt[i];
					s.n++;
					continue;
				}
			}
			break;
		}
		case S_CVT: {
			static const int srcSz[6] = { 4, 4, 4, 4, 4, 8 };
			static const int srcCls[6] = { 0, 0, 0, 0, 1, 1 };
			static const int dstSz[6] = { 8, 8, 4, 8, 8, 4 };
			static const int dstCls[6] = { 0, 0, 1, 1, 1, 1 };
			if (s.sp >= 1) {
				tVE a = s.e[s.sp - 1];
				if (a.sz == srcSz[m.op] && (a.loc != VK_R || a.cls == srcCls[m.op])) {
					if (a.loc == VK_C && (m.op == 0 || (m.op == 1 && (I32)a.val >= 0) || m.op == 2)) {
						// a constant is converted here, not at run time
						if (m.op == 2) { float f = (float)(I32)a.val; memcpy(&a.val, &f, 4); }
						s.e[s.sp - 1].sz = dstSz[m.op]; s.e[s.sp - 1].val = a.val;
						continue;
					}
					if (vst_cvt[m.op][a.loc] >= 0) {
						s.sp--;
						SpillRegister(&s, 0);
						Out(&s, vst_cvt[m.op][a.loc], a.loc == VK_R ? 0 : a.val, 0, 0);
						Push(&s, VK_R, dstSz[m.op], dstCls[m.op], 0);
						continue;
					}
				}
			}
			break;
		}
		default:
			break;
		}
		// not something the pass does: the stencil as it is, with its operands on the stack in memory
		FlushAll(&s);
		CopyIn(&s, l, i);
	}
	if (flushAt[n]) { FlushAll(&s); }
	FlushAll(&s);
	remap[n] = s.n;

	// everything that points into the list now points into the new one
	for (k = 0; k < s.n; k++) {
		if (s.out.tgt[k] >= 0 && (U32)s.out.tgt[k] <= n) { s.out.tgt[k] = (int)remap[s.out.tgt[k]]; }
	}
	memcpy(l->ids, s.out.ids, s.n * sizeof(unsigned));
	memcpy(l->hole0, s.out.hole0, s.n * sizeof(U32)); memcpy(l->hole1, s.out.hole1, s.n * sizeof(U32)); memcpy(l->hole2, s.out.hole2, s.n * sizeof(U32));
	memcpy(l->srcInstr, s.out.srcInstr, s.n * sizeof(U32));
	memcpy(l->iwords, s.out.iwords, s.n * sizeof(tOpWord*));
	memcpy(l->ilen, s.out.ilen, s.n * sizeof(U32)); memcpy(l->ientry, s.out.ientry, s.n * sizeof(U32));
	memcpy(l->endJump, s.out.endJump, s.n);
	memcpy(l->tgt, s.out.tgt, s.n * sizeof(int));
	*pPool = s.pool; *pNumPool = s.numPool;
	{
		U32 total = s.n;
		free(s.out.ids); free(s.out.hole0); free(s.out.hole1); free(s.out.hole2); free(s.out.srcInstr); free(s.out.iwords);
		free(s.out.ilen); free(s.out.ientry); free(s.out.endJump); free(s.out.tgt);
		return total;
	}
}

#else

U32 VStack_Run(tStencilList *l, U32 n, const unsigned char *flushAt, U32 *remap, U64 **pPool, U32 *pNumPool) {
	U32 i;
	*pPool = NULL; *pNumPool = 0;
	for (i = 0; i <= n; i++) { remap[i] = i; }
	return n;
}

#endif
