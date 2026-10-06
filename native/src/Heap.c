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

#include "Heap.h"
#include "HeapTree.h"

#include "MetaData.h"
#include "CLIFile.h"
#include "Type.h"
#include "EvalStack.h"
#include "Finalizer.h"
#include "Thread.h"
#include "System.String.h"
#include "System.Array.h"
#include "System.WeakReference.h"

// Memory roots are:
// All threads, all MethodStates - the ParamLocals memory and the evaluation stack
// All static fields of all types
// Note that the evaluation stack is not typed, so every 4-byte entry is treated as a pointer

typedef struct tSync_ tSync;

struct tSync_ {
	// The thread that holds this sync block
	tThread *pThread;
	// The number of times this thread has entered the sync block
	U32 count;

	// Link to the first weak-ref that targets this object.
	// This allows the tracking of all weak-refs that target this object.
	HEAP_PTR weakRef;
};

// tHeapEntry (the per-allocation header) lives in HeapEntry.h; the address-ordered
// tree that tracks them is in HeapTree.c (see HeapTree.h).

// Get the tHeapEntry pointer when given a HEAP_PTR object
#define GET_HEAPENTRY(heapObj) ((tHeapEntry*)(heapObj - sizeof(tHeapEntry)))

// Forward ref
static void RemoveWeakRefTarget(tHeapEntry *pHeapEntry, U32 removeLongRefs);

#define pHeapTreeRoot HeapTree_RootNode
#define nil HeapTree_NilNode
#define MAX_TREE_DEPTH 40

// The total heap memory currently allocated
static U32 trackHeapSize;
// The max heap size allowed before a garbage collection is triggered
static U32 heapSizeMax;
// Greater than 0 while compiled code runs: no collection (see Heap_SuspendGC)
static U32 gcSuspended;
// The number of allocated memory nodes
static U32 numNodes = 0;
// The number of collections done
static U32 numCollections = 0;
// The lowest and highest address of any heap entry ever allocated: a candidate pointer outside [heapLow, heapHigh] cannot point into the heap,
// which is nearly every integer the collector looks at (it reads the contents of arrays of structs 4 bytes at a time)
static char *heapLow = NULL, *heapHigh = NULL;

#ifdef DIAG_GC
// Track how much time GC's are taking
U64 gcTotalTime = 0;
#endif

// After a collection the next one is scheduled when the heap has grown to about twice the live size, but never
// sooner than MIN_HEAP_SIZE of headroom and never later than MAX_HEAP_EXCESS of it. The ceiling used to be 200,000
// bytes, which for a program whose live heap keeps growing meant a collection every 200 KB of allocation, each one
// marking everything live and sweeping the whole heap: quadratic. (Growing a 60,000-entry Dictionary took 13 s; with
// this ceiling 0.8 s.) A small-memory target can still lower either with -D.
#ifndef MIN_HEAP_SIZE
#define MIN_HEAP_SIZE 50000
#endif
#ifndef MAX_HEAP_EXCESS
#define MAX_HEAP_EXCESS (64U * 1024 * 1024)
#endif

// ================= the allocator =================
// Small objects (the header and memory together up to MAX_SMALL_SLOT bytes: nearly all of them) live in 64 KB chunks that each hold slots of one
// size class. An object is made by taking a slot off its class's free list, or by carving the next one out of the class's current chunk
// (nothing to search, nothing to balance); a pointer into an object finds its slot by one hash lookup of the chunk it is in, and a division.
// A chunk with no live object is given back to a pool, from which any class takes chunks. Large objects are malloc'ed one by one and found through
// the address-ordered tree (HeapTree.c), as every object used to be. DNA_HEAP_MALLOC=1 makes every object large, which is that old behaviour
// (for comparison and for testing the large path).
#define CHUNK_SHIFT 16
#define CHUNK_SIZE ((size_t)1 << CHUNK_SHIFT)
#define MAX_SMALL_SLOT 4096
#define MAX_CLASSES 40

typedef struct tChunk_ tChunk;
struct tChunk_ {
	char *base;                       // CHUNK_SIZE bytes, aligned to CHUNK_SIZE
	U32 slotSize, nSlots;
	U32 carved;                       // how many slots, from the start, have been handed out at least once
	tChunk *next;                     // the next chunk of the same size class
	tChunk *nextPooled;
};
typedef struct {
	U32 slotSize;
	tChunk *first, *last, *cur;       // the chunks of this class; the one that has uncarved slots left
	tHeapEntry *freeHead, *freeTail;  // free slots, linked through the first word of their memory
} tSizeClass;

static tSizeClass classes[MAX_CLASSES];
static U32 numClasses;
static U8 classOf[MAX_SMALL_SLOT / 8 + 1];
static tChunk *chunkPool;
static tChunk **chunkTable;           // chunks by address >> CHUNK_SHIFT: open addressing
static U32 chunkTableSize, chunkCount;
static int allLarge;

static void InitClasses(void) {
	static const U32 sizes[] = { 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
		1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096 };
	U32 minSlot = ((U32)sizeof(tHeapEntry) + (U32)sizeof(void*) + 7) & ~7u, i, t, c = 0;
	numClasses = 0;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (sizes[i] < minSlot) continue;
		classes[numClasses].slotSize = sizes[i];
		numClasses++;
	}
	for (t = 0; t <= MAX_SMALL_SLOT / 8; t++) {
		while (classes[c].slotSize < t * 8) c++;
		classOf[t] = (U8)c;
	}
}

static U32 ChunkHash(uintptr_t key) { return (U32)((key ^ (key >> 17)) * 0x9E3779B1u); }
static void ChunkTableInsert(tChunk *pChunk) {
	U32 i;
	if (chunkCount * 2 >= chunkTableSize) {                       // (grow, and put the chunks in again)
		U32 oldSize = chunkTableSize, k;
		tChunk **old = chunkTable;
		chunkTableSize = oldSize ? oldSize * 2 : 256;
		chunkTable = (tChunk**)calloc(chunkTableSize, sizeof(tChunk*));
		for (k = 0; k < oldSize; k++) {
			if (old[k] != NULL) {
				i = ChunkHash((uintptr_t)old[k]->base >> CHUNK_SHIFT) & (chunkTableSize - 1);
				while (chunkTable[i] != NULL) i = (i + 1) & (chunkTableSize - 1);
				chunkTable[i] = old[k];
			}
		}
		free(old);
	}
	i = ChunkHash((uintptr_t)pChunk->base >> CHUNK_SHIFT) & (chunkTableSize - 1);
	while (chunkTable[i] != NULL) i = (i + 1) & (chunkTableSize - 1);
	chunkTable[i] = pChunk;
	chunkCount++;
}
static tChunk* ChunkFind(const void *p) {
	uintptr_t key = (uintptr_t)p >> CHUNK_SHIFT;
	U32 i;
	if (chunkTableSize == 0) return NULL;
	i = ChunkHash(key) & (chunkTableSize - 1);
	while (chunkTable[i] != NULL) {
		if (((uintptr_t)chunkTable[i]->base >> CHUNK_SHIFT) == key) return chunkTable[i];
		i = (i + 1) & (chunkTableSize - 1);
	}
	return NULL;
}

// A chunk for a size class, from the pool or from new memory (NULL if there is none).
static tChunk* AcquireChunk(tSizeClass *k) {
	tChunk *c = chunkPool;
	if (c != NULL) {
		chunkPool = c->nextPooled;
	} else {
		char *base;
#ifdef _WIN32
		base = (char*)_aligned_malloc(CHUNK_SIZE, CHUNK_SIZE);
#else
		base = (char*)aligned_alloc(CHUNK_SIZE, CHUNK_SIZE);
#endif
		if (base == NULL) return NULL;
		c = (tChunk*)calloc(1, sizeof(tChunk));
		c->base = base;
		ChunkTableInsert(c);
		if (heapLow == NULL || base < heapLow) heapLow = base;
		if (base + CHUNK_SIZE > heapHigh) heapHigh = base + CHUNK_SIZE;
	}
	c->slotSize = k->slotSize; c->nSlots = (U32)(CHUNK_SIZE / k->slotSize); c->carved = 0; c->next = NULL; c->nextPooled = NULL;
	if (k->last != NULL) k->last->next = c; else k->first = c;
	k->last = c;
	return c;
}

// A slot for an object of this many bytes in all (header included), or NULL if memory is out.
static tHeapEntry* AllocSmall(U32 total) {
	tSizeClass *k = &classes[classOf[(total + 7) >> 3]];
	tHeapEntry *e;
	if (k->freeHead != NULL) {
		e = k->freeHead;
		k->freeHead = *(tHeapEntry**)e->memory;
		if (k->freeHead == NULL) k->freeTail = NULL;
		return e;
	}
	if (k->cur == NULL || k->cur->carved == k->cur->nSlots) {
		k->cur = AcquireChunk(k);
		if (k->cur == NULL) return NULL;
	}
	e = (tHeapEntry*)(k->cur->base + (size_t)k->cur->carved * k->slotSize);
	k->cur->carved++;
	return e;
}

// The small object whose slot contains the address p, or NULL (not in a chunk, in a slot that was never used, or one that is free).
static tHeapEntry* FindSmall(const void *p) {
	tChunk *c = ChunkFind(p);
	U32 idx;
	tHeapEntry *e;
	if (c == NULL) return NULL;
	idx = (U32)((size_t)((const char*)p - c->base) / c->slotSize);
	if (idx >= c->carved) return NULL;
	e = (tHeapEntry*)(c->base + (size_t)idx * c->slotSize);
	return e->pTypeDef == NULL ? NULL : e;
}

void Heap_Init() {
	// Initialise vars
	trackHeapSize = 0;
	heapSizeMax = MIN_HEAP_SIZE;
	allLarge = getenv("DNA_HEAP_MALLOC") != NULL;
	InitClasses();
	// Create the (empty) tracking tree and its nil sentinel
	HeapTree_Init();
}

// Get the size of a heap entry, NOT including the header
// This works by returning the size of the type, unless the type is an array or a string,
// which are the only two types that can have variable sizes
static U32 GetSize(tHeapEntry *pHeapEntry) {
	tMD_TypeDef *pType = pHeapEntry->pTypeDef;
	if (pType == types[TYPE_SYSTEM_STRING]) {
		// If it's a string, return the string length in bytes
		return SystemString_GetNumBytes((HEAP_PTR)(pHeapEntry + 1));
	}
	if (TYPE_ISARRAY(pType)) {
		// If it's an array, return the array length * array element size
		return SystemArray_GetNumBytes((HEAP_PTR)(pHeapEntry + 1), pType->pArrayElementType);
	}
	// If it's not string or array, just return the instance memory size
	return pType->instanceMemSize;
}

// The type of the heap object whose memory contains `addr`, or NULL if addr is not inside
// any heap object. Read-only; the same address search the collector uses to find an object.
// Used to tell a pointer into a packed array element from a pointer to a 4-byte field slot.
tMD_TypeDef* Heap_GetObjectTypeContaining(void *addr) {
	tHeapEntry *pEntry = FindSmall(addr);
	tHeapNode *pNode;
	if (pEntry != NULL) {
		return (char*)addr < (char*)pEntry->memory + GetSize(pEntry) ? pEntry->pTypeDef : NULL;
	}
	pNode = pHeapTreeRoot;
	while (pNode != nil) {
		if (addr < (void*)pNode) {
			pNode = pNode->pLink[0];
		} else if ((char*)addr >= ((char*)pNode) + GetSize(NODE_ENTRY(pNode)) + sizeof(tHeapNode)) {
			pNode = pNode->pLink[1];
		} else {
			return pNode->pTypeDef;
		}
	}
	return NULL;
}

// What the sweep makes of an object that the mark phase did not reach: 0 in use (it is unmarked, ready for the next collection),
// 1 not collected yet (it has a finalizer that must run first), 2 dead.
static int Classify(tHeapEntry *e) {
	if (e->marked) {
		if (e->marked != 0xff) {
			// Still in use (but not marked undeletable), so unmark
			e->marked = 0;
		}
		return 0;
	}
	// Not in use any more. If it needs Finalizing, then don't garbage collect, and put in Finalization queue.
	if (e->needToFinalize) {
		if (e->needToFinalize == 1) {
			AddFinalizer((HEAP_PTR)e->memory);
			// Mark it has having been placed in the finalization queue.
			// When it has been finalized, then this will be set to 0
			e->needToFinalize = 2;
			// If this object is being targetted by weak-ref(s), handle it
			if (e->pSync != NULL) {
				RemoveWeakRefTarget(e, 0);
				free(e->pSync);
				e->pSync = NULL;           // (it was left pointing at what was freed, and freed again when the object was finally collected)
			}
		}
		return 1;
	}
	// If this object is being targetted by weak-ref(s), handle it
	if (e->pSync != NULL) {
		RemoveWeakRefTarget(e, 1);
		free(e->pSync);
		e->pSync = NULL;
	}
	return 2;
}

static void GarbageCollect() {
	tHeapRoots heapRoots;
	tHeapNode *pNode;
	tHeapNode *pUp[MAX_TREE_DEPTH * 2];
	I32 top;
	tHeapNode *pToDelete = NULL;
	U32 orgHeapSize = trackHeapSize;
	U32 orgNumNodes = numNodes;
	U32 c;
#ifdef DIAG_GC
	U64 startTime;
#endif

	numCollections++;

#ifdef DIAG_GC
	startTime = microTime();
#endif

	heapRoots.capacity = 64;
	heapRoots.num = 0;
	heapRoots.pHeapEntries = malloc(heapRoots.capacity * sizeof(tHeapRootEntry));

	Thread_GetHeapRoots(&heapRoots);
	CLIFile_GetHeapRoots(&heapRoots);
	Host_GetHeapRoots(&heapRoots);

	// Mark phase
	while (heapRoots.num > 0) {
		tHeapRootEntry *pRootsEntry;
		U32 i;
		U32 moreRootsAdded = 0;
		U32 rootsEntryNumPointers;
		char *pRootsEntryMem;

		// Get a piece of memory off the list of heap memory roots.
		pRootsEntry = &heapRoots.pHeapEntries[heapRoots.num - 1];
		rootsEntryNumPointers = pRootsEntry->numPointers;
		pRootsEntryMem = (char*)pRootsEntry->pMem;
		// Mark this entry as done
		pRootsEntry->numPointers = 0;
		pRootsEntry->pMem = NULL;
		// Iterate through all pointers in it
		for (i=0; i<rootsEntryNumPointers; i++) {
			void *pMemRef;
			tHeapEntry *pEntry;
			memcpy(&pMemRef, pRootsEntryMem + ((size_t)i << 2), sizeof(void*));   // 4-byte steps, maybe unaligned
			// Quick escape for known non-memory (nothing, or outside the addresses of every heap entry: most integers)
			if (pMemRef == NULL || (char*)pMemRef < heapLow || (char*)pMemRef > heapHigh) {
				continue;
			}
			// Find the object that this points into. For a small one: the slot of the byte before it (so that a pointer to just past the end of
			// an object, as an iterator may hold, still holds it up, and the pointer to its memory is in its own slot, after its header).
			// For a large one: the last node that starts at or before it, and the second comparison is <=, not <, to allow for a pointer to the
			// end, and for a zero-sized object (the object class has no memory) to be detected (and not garbage collected) properly.
			pEntry = FindSmall((char*)pMemRef - 1);
			if (pEntry == NULL) {
				tHeapNode *pCandidate = NULL;
				pNode = pHeapTreeRoot;
				while (pNode != nil) {
					if (pMemRef < (void*)pNode) {
						pNode = pNode->pLink[0];
					} else {
						pCandidate = pNode;
						pNode = pNode->pLink[1];
					}
				}
				if (pCandidate != NULL && (char*)pMemRef <= ((char*)pCandidate) + GetSize(NODE_ENTRY(pCandidate)) + sizeof(tHeapNode)) {
					pEntry = NODE_ENTRY(pCandidate);
				}
			}
			if (pEntry != NULL) {
				// Found memory. See if it's already been marked.
				// If it's already marked, then don't do anything.
				// It it's not marked, then add all of its memory to the roots, and mark it.
				if (pEntry->marked == 0) {
					tMD_TypeDef *pType = pEntry->pTypeDef;

					// Not yet marked, so mark it, and add it to heap roots.
					pEntry->marked = 1;

					// Don't look at the contents of strings, arrays of primitive types, or WeakReferences
					if (pType->stackType == EVALSTACK_O ||
						pType->stackType == EVALSTACK_VALUETYPE ||
						pType->stackType == EVALSTACK_PTR) {

						if (pType != types[TYPE_SYSTEM_STRING] &&
							(!TYPE_ISARRAY(pType) ||
							pType->pArrayElementType->stackType == EVALSTACK_O ||
							pType->pArrayElementType->stackType == EVALSTACK_VALUETYPE ||
							pType->pArrayElementType->stackType == EVALSTACK_PTR)) {

							if (pType != types[TYPE_SYSTEM_WEAKREFERENCE]) {
								Heap_SetRoots(&heapRoots, pEntry->memory, GetSize(pEntry));
								moreRootsAdded = 1;
							}
						}
					}
				}
			}
		}
		if (!moreRootsAdded) {
			heapRoots.num--;
		}
	}

	free(heapRoots.pHeapEntries);

	// Sweep phase. First every object is sorted into live, to be finalized, or dead; then the dead ones are freed. (They are all still whole
	// while the others are looked at: a weak reference's handling reads the objects it names.)
	// Small objects: slot by slot, in the order of memory
	for (c = 0; c < numClasses; c++) {
		tChunk *pChunk;
		for (pChunk = classes[c].first; pChunk != NULL; pChunk = pChunk->next) {
			U32 idx;
			for (idx = 0; idx < pChunk->carved; idx++) {
				tHeapEntry *e = (tHeapEntry*)(pChunk->base + (size_t)idx * pChunk->slotSize);
				if (e->pTypeDef != NULL && Classify(e) == 2) {
					e->marked = 0xfe;                          // dead
				}
			}
		}
	}
	// Large objects: the nodes of the tree
	if (pHeapTreeRoot != nil) {
		pUp[0] = pHeapTreeRoot;
		top = 1;
		while (top != 0) {
			// Get this node
			pNode = pUp[--top];
			// Act on this node
			if (Classify(NODE_ENTRY(pNode)) == 2) {
				// Use pSync to point to next entry in this linked-list.
				pNode->pSync = (tSync*)pToDelete;
				pToDelete = pNode;
			}
			// Get next node(s)
			if (pNode->pLink[1] != nil) {
				pUp[top++] = pNode->pLink[1];
			}
			if (pNode->pLink[0] != nil) {
				pUp[top++] = pNode->pLink[0];
			}
		}
	}

	// Free what is dead. Small objects: each class's free list is made again from the free slots of the chunks that are still in use, and a
	// chunk that has nothing live in it goes to the pool.
	for (c = 0; c < numClasses; c++) {
		tSizeClass *k = &classes[c];
		tChunk *pChunk = k->first, *pPrev = NULL, *pNext;
		k->freeHead = k->freeTail = NULL;
		while (pChunk != NULL) {
			U32 idx, live = 0;
			pNext = pChunk->next;
			for (idx = 0; idx < pChunk->carved; idx++) {
				tHeapEntry *e = (tHeapEntry*)(pChunk->base + (size_t)idx * pChunk->slotSize);
				if (e->pTypeDef != NULL && e->marked == 0xfe) {
					trackHeapSize -= GetSize(e) + sizeof(tHeapEntry);
					numNodes--;
					e->pTypeDef = NULL; e->marked = 0; e->needToFinalize = 0; e->pSync = NULL;
				}
				if (e->pTypeDef != NULL) live++;
			}
			if (live == 0) {
				if (pPrev != NULL) pPrev->next = pNext; else k->first = pNext;
				if (k->last == pChunk) k->last = pPrev;
				pChunk->carved = 0;
				pChunk->nextPooled = chunkPool;
				chunkPool = pChunk;
			} else {
				for (idx = 0; idx < pChunk->carved; idx++) {
					tHeapEntry *e = (tHeapEntry*)(pChunk->base + (size_t)idx * pChunk->slotSize);
					if (e->pTypeDef == NULL) {
						*(tHeapEntry**)e->memory = NULL;
						if (k->freeTail != NULL) *(tHeapEntry**)k->freeTail->memory = e; else k->freeHead = e;
						k->freeTail = e;
					}
				}
				pPrev = pChunk;
			}
			pChunk = pNext;
		}
		k->cur = (k->last != NULL && k->last->carved < k->last->nSlots) ? k->last : NULL;
	}
	// Large objects
	while (pToDelete != NULL) {
		tHeapNode *pThis = pToDelete;
		pToDelete = (tHeapNode*)(pToDelete->pSync);
		HeapTree_Remove(pThis);
		numNodes--;
		trackHeapSize -= GetSize(NODE_ENTRY(pThis)) + sizeof(tHeapEntry);
		free(pThis);
	}

#ifdef DIAG_GC
	gcTotalTime += microTime() - startTime;
#endif

	log_f(1, "--- GARBAGE --- [Size: %d -> %d] [Nodes: %d -> %d]\n",
		orgHeapSize, trackHeapSize, orgNumNodes, numNodes);

#ifdef DIAG_GC
	log_f(1, "GC time = %d ms\n", gcTotalTime / 1000);
#endif
}

void Heap_UnmarkFinalizer(HEAP_PTR heapPtr) {
	((tHeapEntry*)(heapPtr - sizeof(tHeapEntry)))->needToFinalize = 0;
}

void Heap_GarbageCollect() {
	GarbageCollect();
}

U32 Heap_NumCollections() {
	return numCollections;
}

U32 Heap_GetTotalMemory() {
	return trackHeapSize;
}

void Heap_SetRoots(tHeapRoots *pHeapRoots, void *pRoots, U32 sizeInBytes) {
	tHeapRootEntry *pRootEntry;

	Assert((sizeInBytes & 0x3) == 0);
	if (pHeapRoots->num >= pHeapRoots->capacity) {
		pHeapRoots->capacity <<= 1;
		pHeapRoots->pHeapEntries = (tHeapRootEntry*)realloc(pHeapRoots->pHeapEntries, pHeapRoots->capacity * sizeof(tHeapRootEntry));
	}
	pRootEntry = &pHeapRoots->pHeapEntries[pHeapRoots->num++];
	// The 4-byte steps at which a whole pointer still fits inside the region
	pRootEntry->numPointers = (sizeInBytes >= sizeof(void*)) ? ((sizeInBytes - sizeof(void*)) >> 2) + 1 : 0;
	pRootEntry->pMem = pRoots;
}

void Heap_SuspendGC(void) {
	// DNA_UNSAFE_GC=1 does not suspend: a control for the tests, which must then fail (compiled code's references are invisible to a collection)
	static int unsafe = -1;
	if (unsafe < 0) unsafe = getenv("DNA_UNSAFE_GC") != NULL;
	if (!unsafe) gcSuspended++;
}

void Heap_ResumeGC(void) {
	static int stress = -1;
	if (stress < 0) stress = getenv("DNA_GC_STRESS") != NULL;
	if (gcSuspended > 0) gcSuspended--;
	if (gcSuspended == 0 && (trackHeapSize >= heapSizeMax || stress)) {
		// the collection that the allocations made meanwhile would have done, with the same limits Heap_Alloc then sets
		GarbageCollect();
		heapSizeMax = trackHeapSize << 1;
		if (heapSizeMax < trackHeapSize + MIN_HEAP_SIZE) heapSizeMax = trackHeapSize + MIN_HEAP_SIZE;
		if (heapSizeMax > trackHeapSize + MAX_HEAP_EXCESS) heapSizeMax = trackHeapSize + MAX_HEAP_EXCESS;
	}
}

HEAP_PTR Heap_Alloc(tMD_TypeDef *pTypeDef, U32 size) {
	tHeapEntry *pEntry;
	U32 totalSize;

	totalSize = sizeof(tHeapEntry) + size;

	// Trigger garbage collection if required.
	if (trackHeapSize >= heapSizeMax && gcSuspended == 0) {
		GarbageCollect();
		heapSizeMax = (trackHeapSize + totalSize) << 1;
		if (heapSizeMax < trackHeapSize + totalSize + MIN_HEAP_SIZE) {
			// Make sure there is always MIN_HEAP_SIZE available to allocate on the heap
			heapSizeMax = trackHeapSize + totalSize + MIN_HEAP_SIZE;
		}
		if (heapSizeMax > trackHeapSize + totalSize + MAX_HEAP_EXCESS) {
			// Make sure there is never more that MAX_HEAP_EXCESS space on the heap
			heapSizeMax = trackHeapSize + totalSize + MAX_HEAP_EXCESS;
		}
	}

	if (totalSize <= MAX_SMALL_SLOT && !allLarge) {
		pEntry = AllocSmall(totalSize);
		if (pEntry == NULL && gcSuspended == 0) {                // out of memory: collect and try again
			GarbageCollect();
			pEntry = AllocSmall(totalSize);
		}
		if (pEntry == NULL) {
			Crash("Out of memory: could not allocate %u bytes on the managed heap", (unsigned)size);
		}
		pEntry->level = 0;                                       // (unused in a small object)
	} else {
		tHeapNode *pNode = (tHeapNode*)malloc(sizeof(tHeapNode) + size);
		if (pNode == NULL && gcSuspended == 0) {
			GarbageCollect();
			pNode = (tHeapNode*)malloc(sizeof(tHeapNode) + size);
		}
		if (pNode == NULL) {
			Crash("Out of memory: could not allocate %u bytes on the managed heap", (unsigned)size);
		}
		pEntry = NODE_ENTRY(pNode);
		if (heapLow == NULL || (char*)pNode < heapLow) { heapLow = (char*)pNode; }
		if ((char*)pNode + sizeof(tHeapNode) + size > heapHigh) { heapHigh = (char*)pNode + sizeof(tHeapNode) + size; }
		HeapTree_Insert(pNode);
	}
	pEntry->marked = 0;
	pEntry->padding = 0;
	pEntry->pTypeDef = pTypeDef;
	pEntry->pSync = NULL;
	pEntry->needToFinalize = (pTypeDef->pFinalizer != NULL);
	memset(pEntry->memory, 0, size);
	trackHeapSize += totalSize;
	numNodes++;

	return pEntry->memory;
}

HEAP_PTR Heap_AllocType(tMD_TypeDef *pTypeDef) {
	//printf("Heap_AllocType('%s')\n", pTypeDef->name);
	return Heap_Alloc(pTypeDef, pTypeDef->instanceMemSize);
}

tMD_TypeDef* Heap_GetType(HEAP_PTR heapEntry) {
	tHeapEntry *pHeapEntry = GET_HEAPENTRY(heapEntry);
	return pHeapEntry->pTypeDef;
}

void Heap_MakeUndeletable(HEAP_PTR heapEntry) {
	tHeapEntry *pHeapEntry = GET_HEAPENTRY(heapEntry);
	pHeapEntry->marked = 0xff;
}

void Heap_MakeDeletable(HEAP_PTR heapEntry) {
	tHeapEntry *pHeapEntry = GET_HEAPENTRY(heapEntry);
	pHeapEntry->marked = 0;
}

HEAP_PTR Heap_Box(tMD_TypeDef *pType, PTR pMem) {
	HEAP_PTR boxed;

	boxed = Heap_AllocType(pType);
	memcpy(boxed, pMem, pType->instanceMemSize);

	return boxed;
}

HEAP_PTR Heap_Clone(HEAP_PTR obj) {
	tHeapEntry *pObj = GET_HEAPENTRY(obj);
	HEAP_PTR clone;
	U32 size = GetSize(pObj);

	clone = Heap_Alloc(pObj->pTypeDef, size);
	memcpy(clone, pObj->memory, size);

	return clone;
}

static tSync* EnsureSync(tHeapEntry *pHeapEntry) {
	if (pHeapEntry->pSync == NULL) {
		tSync *pSync = TMALLOC(tSync);
		memset(pSync, 0, sizeof(tSync));
		pHeapEntry->pSync = pSync;
	}
	return pHeapEntry->pSync;
}

static void DeleteSync(tHeapEntry *pHeapEntry) {
	if (pHeapEntry->pSync != NULL) {
		if (pHeapEntry->pSync->count == 0 && pHeapEntry->pSync->weakRef == NULL) {
			free(pHeapEntry->pSync);
			pHeapEntry->pSync = NULL;
		}
	}
}

// Return 1 if lock succesfully got
// Return 0 if couldn't get the lock this time
U32 Heap_SyncTryEnter(HEAP_PTR obj) {
	tHeapEntry *pHeapEntry = GET_HEAPENTRY(obj);
	tThread *pThread = Thread_GetCurrent();
	tSync *pSync;

	pSync = EnsureSync(pHeapEntry);
	if (pSync->pThread == NULL) {
		pSync->pThread = pThread;
		pSync->count = 1;
		return 1;
	}
	if (pSync->pThread == pThread) {
		pSync->count++;
		return 1;
	}
	return 0;
}

// Returns 1 if all is OK
// Returns 0 if the wrong thread is releasing the sync, or if no thread hold the sync
U32 Heap_SyncExit(HEAP_PTR obj) {
	tHeapEntry *pHeapEntry = GET_HEAPENTRY(obj);
	tThread *pThread = Thread_GetCurrent();
	if (pHeapEntry->pSync == NULL) {
		return 0;
	}
	if (pHeapEntry->pSync->pThread != pThread) {
		return 0;
	}
	if (--pHeapEntry->pSync->count == 0) {
		DeleteSync(pHeapEntry);
	}
	return 1;
}

static void RemoveWeakRefTarget(tHeapEntry *pTarget, U32 removeLongRefs) {
	SystemWeakReference_TargetGone(&pTarget->pSync->weakRef, removeLongRefs);
}

// Returns the previous first weak-ref in target targetted by weakref
HEAP_PTR Heap_SetWeakRefTarget(HEAP_PTR target, HEAP_PTR weakRef) {
	tHeapEntry *pTarget = GET_HEAPENTRY(target);
	tSync *pSync;
	HEAP_PTR prevWeakRef;

	pSync = EnsureSync(pTarget);
	prevWeakRef = pSync->weakRef;
	pSync->weakRef = weakRef;
	return prevWeakRef;
}

HEAP_PTR* Heap_GetWeakRefAddress(HEAP_PTR target) {
	tHeapEntry *pTarget = GET_HEAPENTRY(target);
	return &pTarget->pSync->weakRef;
}

void Heap_RemovedWeakRefTarget(HEAP_PTR target) {
	tHeapEntry *pTarget = GET_HEAPENTRY(target);
	DeleteSync(pTarget);
}
