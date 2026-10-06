// Heap entry header, shared by Heap.c and HeapTree.c.

#if !defined(__HEAPENTRY_H)
#define __HEAPENTRY_H

#include <stddef.h>

struct tMD_TypeDef_;
struct tSync_;

// A heap object is its memory, with this just before it (GET_HEAPENTRY in Heap.c): 12 bytes on a 32-bit target, 24 on a 64-bit one.
typedef struct tHeapEntry_ tHeapEntry;
struct tHeapEntry_ {
	// (for a large object: the 'level' of its node in the tree; unused for a small one. It is here so that the fields that follow are at the
	// same distance from the object's memory whichever kind of object it is)
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

	// The type in this heap entry. NULL in a slot of a small-object chunk that is free.
	struct tMD_TypeDef_ *pTypeDef;

	// Used for locking sync, and tracking WeakReference that point to this object
	struct tSync_ *pSync;

	// The user memory
	unsigned char memory[0];
};

// A large object (see Heap.c) is found through an address-ordered balanced tree (HeapTree.c), whose links are in a node that comes before the
// entry: this is the entry's own layout with the two links in front, so the entry is at &node->level.
typedef struct tHeapNode_ tHeapNode;
struct tHeapNode_ {
	// Left/right links in the heap binary tree
	tHeapNode *pLink[2];
	// The 'level' of this node. Leaf nodes have lowest level
	unsigned char level;
	unsigned char marked;
	unsigned char needToFinalize;
	unsigned char padding;
	struct tMD_TypeDef_ *pTypeDef;
	struct tSync_ *pSync;
	unsigned char memory[0];
};
#define NODE_ENTRY(node) ((tHeapEntry*)&(node)->level)
#define ENTRY_NODE(entry) ((tHeapNode*)((char*)(entry) - offsetof(tHeapNode, level)))

#endif
