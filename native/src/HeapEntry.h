// Heap entry header, shared by Heap.c (C) and cpp/HeapTree.cpp (Crust C++ subset).
// Keep this file in the intersection of C99 and the cpprust subset.

#if !defined(__HEAPENTRY_H)
#define __HEAPENTRY_H

struct tMD_TypeDef_;
struct tSync_;

typedef struct tHeapEntry_ tHeapEntry;

// The memory is kept track of using a balanced binary search tree (ordered by
// memory address). See HeapTree.h.
struct tHeapEntry_ {
	// Left/right links in the heap binary tree
	tHeapEntry *pLink[2];
	// The 'level' of this node. Leaf nodes have lowest level
	unsigned char level;
	// Used to mark that this node is still in use.
	// If this is set to 0xff, then this heap entry is undeletable.
	unsigned char marked;
	// Set to 1 if the Finalizer needs to be run.
	// Set to 2 if this has been added to the Finalizer queue
	// Set to 0 when the Finalizer has been run (or there is no Finalizer in the first place)
	// Only set on types that have a Finalizer
	unsigned char needToFinalize;

	// unused
	unsigned char padding;

	// The type in this heap entry
	struct tMD_TypeDef_ *pTypeDef;

	// Used for locking sync, and tracking WeakReference that point to this object
	struct tSync_ *pSync;

	// The user memory
	unsigned char memory[0];
};

#endif
