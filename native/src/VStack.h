// The register pass of the native block compiler (VStack.c): it rewrites the list of plain stencils that EmitBlock makes, which keep every value
// on the evaluation stack in memory, into one that keeps them in registers where it can, using the three-address stencils that
// tools/gen_vstencils.py makes.

#if !defined (__VSTACK_H)
#define __VSTACK_H

#include "Types.h"
#include "JIT.h"

// The list of stencils of a block, as parallel arrays (EmitBlock's): the stencil, its operands (HOLE0, HOLE3, HOLE4), the instruction a branch
// came from, what an island is, where a branch goes inside a recipe that is being inlined, and so on. VStack_Run moves them together.
typedef struct {
	unsigned *ids;
	U32 *hole0, *hole1, *hole2;
	U32 *srcInstr;
	const tOpWord **iwords;
	U32 *ilen, *ientry;
	char *endJump;
	int *tgt;
} tStencilList;

// Rewrite `l` (n stencils) in place. flushAt[i] (i <= n) says that nothing may be held in a register when stencil i is reached, because
// something can jump or be entered there; at n it is the end of the block, where everything is on the stack again. remap[0..n] gets the
// new position of each old one. Returns the new length (never more than n).
// 64-bit constants the new list reads are in the pool that is returned in *pPool (malloc'd, *pNumPool of them: the caller frees it).
U32 VStack_Run(tStencilList *l, U32 n, const unsigned char *flushAt, U32 *remap, U64 **pPool, U32 *pNumPool);

#endif
