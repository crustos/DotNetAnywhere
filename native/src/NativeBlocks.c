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

#include "Compat.h"
#include "Sys.h"

#include "NativeBlocks.h"

#if NATIVE_BLOCKS

#include <sys/mman.h>
#include <string.h>

#define STENCIL_DATA
#ifdef STENCILS_HEADER
#include STENCILS_HEADER          // (build.py --ffi: the header with the stencils that call C functions as well)
#else
#include "Stencils.gen.h"
#endif
#undef STENCIL_DATA

#include <stddef.h>
#include "System.Array.h"
// the array stencils (stencils.c) hard-code this layout: the length at offset 0, the elements from offset 4
typedef char array_layout_is_what_the_stencils_assume[(offsetof(tSystemArray, length) == 0 && offsetof(tSystemArray, elements) == 4) ? 1 : -1];

#define POOL_SIZE (256 * 1024)

static unsigned char *pool = NULL;
static unsigned poolUsed = 0;

// DNA_STENCIL_STATS=1: at exit, how many blocks were compiled and how many of each stencil they used
static unsigned numBlocks = 0, numInlined = 0, numIslandsMade = 0, stencilUses[ST_COUNT];
static void PrintStats(void) {
	unsigned i;
	fprintf(stderr, "native blocks compiled: %u; calls inlined into blocks: %u; islands in blocks: %u\n", numBlocks, numInlined, numIslandsMade);
	for (i = 0; i < ST_COUNT; i++) {
		fprintf(stderr, "  %-8s %u\n", stencilNames[i], stencilUses[i]);
	}
}

int NativeBlock_FindStencil(const char *name) {
	unsigned i;
	for (i = 0; i < ST_COUNT; i++) {
		if (strcmp(stencilNames[i], name) == 0) { return (int)i; }
	}
	return -1;
}

void NativeBlock_CountInlined(void) { numInlined++; }
void NativeBlock_CountIslands(U32 n) { numIslandsMade += n; }

void NativeBlock_Dump(const unsigned *ids, const U32 *holes, const int *tgt, U32 n) {
	U32 i;
	fprintf(stderr, "  block of %u stencils:", n);
	for (i = 0; i < n; i++) {
		fprintf(stderr, "%s%u:%s", i % 8 == 0 ? "\n   " : " ", i, stencilNames[ids[i]]);
		if (holes[i] != 0 || tgt[i] >= 0) { fprintf(stderr, "(%d%s", (int)holes[i], tgt[i] >= 0 ? "" : ")"); }
		if (tgt[i] >= 0) { fprintf(stderr, "->%d)", tgt[i]); }
	}
	fprintf(stderr, "\n");
}

int NativeBlock_Enabled(void) {
	static int enabled = -1;
	if (enabled < 0) {
		enabled = (getenv("DNA_NO_STENCILS") == NULL);
	}
	return enabled;
}

// Memory the blocks live in. Blocks are never freed (a method's JITted code lives as long as the program does).
static unsigned char* PoolAlloc(unsigned size) {
	if (pool == NULL || poolUsed + size > POOL_SIZE) {
		void *p = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED || size > POOL_SIZE) {
			return NULL;
		}
		pool = (unsigned char*)p;
		poolUsed = 0;
	}
	{
		unsigned char *r = pool + poolUsed;
		poolUsed += (size + 15) & ~15u;
		return r;
	}
}

// The layout of a block:
//   prologue (only if there is more than one entry): for each entry e >= 1,  cmp eax,e; je <entry e>     (3 + 6 bytes each)
//   the stencils
//   done:      xor eax,eax; ret                                              (3)
//   null:      mov eax,NB_NULLREF; ret                                       (6)
//   index:     mov eax,NB_INDEXRANGE; ret                                    (6)
//   restart e: mov eax,NB_RESTART+e; ret                                     (6 each, one per entry)
//   exit k:    mov eax,NB_EXIT+k; ret                                        (6 each)
//   one trampoline per backward branch:  dec r14; jz restart e; jmp target   (3 + 6 + 5 = 14 each)
#define DONE_SIZE 3
#define STUB_SIZE 6
#define TRAMPOLINE_SIZE 14
#define ENTRY_DISPATCH_SIZE 9

static void PutStub(unsigned char *p, U32 status) {
	p[0] = 0xB8;                        // mov eax, imm32
	memcpy(p + 1, &status, 4);
	p[5] = 0xC3;                        // ret
}

static void PutRel32(unsigned char *field, const unsigned char *target, int addend) {
	// what the linker would write for a PC-relative reference: target + addend - the address of the field
	I32 rel = (I32)((intptr_t)target + addend - (intptr_t)field);
	memcpy(field, &rel, 4);
}

void* NativeBlock_Compile(const unsigned *ids, const U32 *hole0, const U32 *hole1, const U32 *hole2, const int *tgt, U32 n, U32 numExits,
		const U32 *entries, U32 numEntries, const U64 *pool64, U32 numPool) {
	U32 i, h, e, k, numBack = 0, prologue, bodyLen = 0, total, poolOffset;
	unsigned char *poolBase;
	U32 *off;
	unsigned char *code, *w, *done, *nullStub, *indexStub, *restartStubs, *exits, *tramp;

	if (!NativeBlock_Enabled() || numEntries > 100) {
		return NULL;
	}
	prologue = numEntries > 1 ? ENTRY_DISPATCH_SIZE * (numEntries - 1) : 0;
	off = (U32*)malloc((n + 1) * sizeof(U32));
	for (i = 0; i < n; i++) {
		off[i] = prologue + bodyLen;
		bodyLen += stencils[ids[i]].len;
		if (tgt[i] >= 0 && (U32)tgt[i] < n && (U32)tgt[i] <= i) {
			numBack++;
		}
	}
	total = prologue + bodyLen + DONE_SIZE + STUB_SIZE * (2 + numEntries + numExits) + TRAMPOLINE_SIZE * numBack;
	poolOffset = (total + 7) & ~7u;                       // the constants after all that, 8-aligned
	total = poolOffset + 8 * numPool;
	code = PoolAlloc(total + 8);
	if (code != NULL) {
		// (PoolAlloc hands out 16-aligned memory, so 8-aligned offsets are 8-aligned addresses)
		poolBase = code + poolOffset;
		memcpy(poolBase, pool64, 8 * numPool);
	}
	if (code == NULL) {
		free(off);
		return NULL;
	}
	if (numBlocks == 0 && getenv("DNA_STENCIL_STATS") != NULL) {
		atexit(PrintStats);
	}
	numBlocks++;
	done = code + prologue + bodyLen;
	nullStub = done + DONE_SIZE;
	indexStub = nullStub + STUB_SIZE;
	restartStubs = indexStub + STUB_SIZE;
	exits = restartStubs + STUB_SIZE * numEntries;
	tramp = exits + STUB_SIZE * numExits;

	for (e = 1; e < numEntries; e++) {
		w = code + ENTRY_DISPATCH_SIZE * (e - 1);
		w[0] = 0x83; w[1] = 0xF8; w[2] = (unsigned char)e;                  // cmp eax, e
		w[3] = 0x0F; w[4] = 0x84; PutRel32(w + 5, code + off[entries[e]], -4);   // je entry e
	}
	done[0] = 0x31; done[1] = 0xC0; done[2] = 0xC3;       // xor eax,eax; ret
	PutStub(nullStub, NB_NULLREF);
	PutStub(indexStub, NB_INDEXRANGE);
	for (e = 0; e < numEntries; e++) {
		PutStub(restartStubs + STUB_SIZE * e, NB_RESTART + e);
	}
	for (k = 0; k < numExits; k++) {
		PutStub(exits + STUB_SIZE * k, NB_EXIT + k);
	}

	for (i = 0; i < n; i++) {
		const tStencil *s = &stencils[ids[i]];
		w = code + off[i];
		memcpy(w, s->code, s->len);
		stencilUses[ids[i]]++;
		for (h = 0; h < s->numHoles; h++) {
			if (s->holes[h].rel) {
				const unsigned char *dest;
				if (s->holes[h].hole == 5) {
					dest = poolBase + 8 * hole2[i];    // HOLE5: a constant of the pool
				} else if (s->holes[h].hole == 2) {
					dest = indexStub;                  // HOLE2 is always "the index is out of range"
				} else if (tgt[i] < 0) {
					dest = nullStub;
				} else if ((U32)tgt[i] >= n) {
					dest = exits + STUB_SIZE * ((U32)tgt[i] - n);
				} else if ((U32)tgt[i] > i) {
					dest = code + off[tgt[i]];
				} else {
					// a backward branch: through a trampoline that counts the iteration, then goes to the target. If the
					// count runs out, the block gives up the processor and is later entered again at that target.
					U32 entry = 0;
					for (e = 0; e < numEntries; e++) {
						if (entries[e] == (U32)tgt[i]) { entry = e; break; }
					}
					dest = tramp;
					*tramp++ = 0x49; *tramp++ = 0xFF; *tramp++ = 0xCE;                                          // dec r14
					*tramp++ = 0x0F; *tramp++ = 0x84; PutRel32(tramp, restartStubs + STUB_SIZE * entry, -4); tramp += 4;   // jz restart
					*tramp++ = 0xE9; PutRel32(tramp, code + off[tgt[i]], -4); tramp += 4;                      // jmp target
				}
				PutRel32(w + s->holes[h].off, dest, s->holes[h].addend);
			} else {
				// the field holds (hole value + addend), as the linker would have written it
				// HOLE0 is the first operand, HOLE3 the second, HOLE4 the third
				U32 v = (s->holes[h].hole == 3 ? hole1[i] : s->holes[h].hole == 4 ? hole2[i] : hole0[i]) + (U32)s->holes[h].addend;
				memcpy(w + s->holes[h].off, &v, 4);
			}
		}
	}
	free(off);
	return code;
}

#else

int NativeBlock_Enabled(void) { return 0; }
void NativeBlock_CountInlined(void) { }
int NativeBlock_FindStencil(const char *name) { return -1; }
void NativeBlock_CountIslands(U32 n) { }
void NativeBlock_Dump(const unsigned *ids, const U32 *holes, const int *tgt, U32 n) { }
void* NativeBlock_Compile(const unsigned *ids, const U32 *hole0, const U32 *hole1, const U32 *hole2, const int *tgt, U32 n, U32 numExits,
	const U32 *entries, U32 numEntries, const U64 *pool, U32 numPool) { return NULL; }

#endif
