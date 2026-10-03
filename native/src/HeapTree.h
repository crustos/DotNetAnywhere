// C API for the heap-tracking tree. Implemented in cpp/HeapTree.cpp, which is
// written in the Crust C++ subset and lowered to C by tools/cpprust.py
// (see build.py).

#if !defined(__HEAPTREE_H)
#define __HEAPTREE_H

#include "HeapEntry.h"

// Create the empty tree and its nil sentinel.
void HeapTree_Init(void);
// The sentinel that terminates every leaf. A plain global (set by
// HeapTree_Init) because the GC compares against it in its inner loops.
extern tHeapEntry *HeapTree_NilNode;
// Root of the tree.
tHeapEntry* HeapTree_Root(void);
// Add / remove a node (ordered by address).
void HeapTree_Insert(tHeapEntry *pEntry);
void HeapTree_Remove(tHeapEntry *pEntry);

#endif
