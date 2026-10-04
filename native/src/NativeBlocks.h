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

#if !defined(__NATIVEBLOCKS_H)
#define __NATIVEBLOCKS_H

// Native blocks: a run of simple instructions (loads and stores of locals, float32 arithmetic) compiled to machine
// code by copying and patching the stencils in Stencils.gen.h, and called from the interpreter as one instruction.
// x86-64 Linux only; DNA_NO_STENCILS=1 turns it off. See tools/gen_stencils.py and native/stencils/stencils.c.

#include "Types.h"
#ifdef STENCILS_HEADER
#include STENCILS_HEADER          // (build.py --ffi: the header with the stencils that call C functions as well)
#else
#include "Stencils.gen.h"
#endif

#if STENCILS_AVAILABLE && defined(__x86_64__) && defined(__linux__)
#define NATIVE_BLOCKS 1
#else
#define NATIVE_BLOCKS 0
#endif

// Print a block's stencils, their holes and where they branch (DNA_FUSION_DEBUG)
void NativeBlock_Dump(const unsigned *ids, const U32 *holes, const int *tgt, U32 n);

// The stencil with this name (st_<name> in the stencil source), or -1: how the generated FFI stencils are found
int NativeBlock_FindStencil(const char *name);

// Count the islands of a block (for DNA_STENCIL_STATS)
void NativeBlock_CountIslands(U32 n);

// Count a call that a block contains the callee of (for DNA_STENCIL_STATS)
void NativeBlock_CountInlined(void);

// Are native blocks in use? (They are, unless unsupported here or switched off.)
int NativeBlock_Enabled(void);

// How a block finished (its return value)
#define NB_DONE 0        // ran to the end: carry on with the instruction after the block
#define NB_NULLREF 1     // a field or an array was accessed through a null reference: the interpreter must throw
#define NB_INDEXRANGE 2  // an array index was out of range: the interpreter must throw IndexOutOfRangeException
#define NB_RESTART 0x10000   // NB_RESTART + e: the iteration budget ran out at a backward branch to entry e: yield, and
                             // run the block again from that entry
#define NB_EXIT 0x20000      // NB_EXIT + k: the block's k'th exit was taken: carry on at that exit's target

// Where a branch stencil goes, in the `tgt` array of NativeBlock_Compile (stencil i's HOLE1):
//   -1                  the null-reference stub (every stencil that can fail on null uses this)
//   0 .. n-1            stencil number j of this block (a backward one, j <= i, goes through the iteration counter)
//   n + k               the block's k'th exit
// Make a block from `n` stencils (ST_* ids), the i'th with HOLE0 = hole0[i]. A block can be entered at its start (entry
// 0) and at the stencils named in entries[1 .. numEntries-1] (the targets of its backward branches). Returns the code, or
// NULL if it cannot.
// A stencil may have more than one value hole: HOLE0 takes hole0[i], HOLE3 takes hole1[i] and HOLE4 hole2[i] (the three operands of a
// three-address stencil: two sources and a destination). hole1 and hole2 may be NULL if no stencil uses them.
// HOLE5 is a RIP-relative read of the block's constant pool, the entry hole2[i] of `pool` (64-bit constants; VStack.c).
void* NativeBlock_Compile(const unsigned *ids, const U32 *hole0, const U32 *hole1, const U32 *hole2, const int *tgt, U32 n, U32 numExits,
	const U32 *entries, U32 numEntries, const U64 *pool, U32 numPool);

#if NATIVE_BLOCKS
// Run a block that has no loop: the same, without the budget and the entry (it never gives up the processor, so it is
// leaner, and most blocks are like this).
static inline U32 NativeBlock_RunStraight(void *code, PTR fp, PTR *pSp) {
	register PTR sp12 __asm__("r12") = *pSp;
	register PTR fp13 __asm__("r13") = fp;
	U32 status;
	__asm__ volatile("lea -128(%%rsp), %%rsp\n\tcall *%[code]\n\tlea 128(%%rsp), %%rsp"
		: "+r"(sp12), "=a"(status)
		: "r"(fp13), [code] "r"(code)
		: "memory", "cc", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
		  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
	*pSp = sp12;
	return status;
}

// Run a block. The evaluation stack pointer goes in r12 and the frame in r13, which is how the stencils expect them;
// the new stack pointer comes back in r12 and the status in eax. r14 is the iteration budget: every backward branch
// taken costs one, and when it reaches zero the block returns NB_RESTART + entry; what is left comes back in *pBudget.
// `entry` (in eax) says where to start: 0 is the beginning, anything else a backward-branch target. The code
// is plain leaf code that uses no stack but its return address, so besides the registers named it only needs the 128
// bytes below rsp (the red zone) to be left alone.
static inline U32 NativeBlock_Run(void *code, PTR fp, PTR *pSp, U32 *pBudget, U32 entry) {
	register PTR sp12 __asm__("r12") = *pSp;
	register PTR fp13 __asm__("r13") = fp;
	register unsigned long budget14 __asm__("r14") = *pBudget;
	U32 status = entry;
	__asm__ volatile("lea -128(%%rsp), %%rsp\n\tcall *%[code]\n\tlea 128(%%rsp), %%rsp"
		: "+r"(sp12), "+r"(budget14), "+a"(status)
		: "r"(fp13), [code] "r"(code)
		: "memory", "cc", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
		  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
	*pSp = sp12;
	*pBudget = (U32)budget14;
	return status;
}
#endif

#endif
