// The heap-tracking tree (HeapTree.c): an AA tree of the heap entries, ordered by address.

#if !defined(__HEAPTREE_H)
#define __HEAPTREE_H

#include "HeapEntry.h"

// Create the empty tree and its nil sentinel.
void HeapTree_Init(void);
// The sentinel that terminates every leaf. A plain global (set by HeapTree_Init) because the GC
// compares against it in its inner loops.
extern tHeapNode *HeapTree_NilNode;
// The root of the tree (HeapTree_NilNode if it is empty). Read it, do not set it: Insert and Remove do.
extern tHeapNode *HeapTree_RootNode;
// Add / remove a node (ordered by address). An entry is initialised (level, links, mark) as it is added.
void HeapTree_Insert(tHeapNode *pEntry);
void HeapTree_Remove(tHeapNode *pEntry);

#endif
