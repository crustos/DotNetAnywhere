// The heap-tracking tree: an Andersson (AA) tree over heap entries, ordered by memory address.
// See http://www.eternallyconfuzzled.com/tuts/datastructures/jsw_tut_andersson.aspx
//
// The entries are the heap objects' own headers (tHeapNode), so the tree allocates nothing: a node is linked in
// when the object is allocated (HeapTree_Insert) and out when it is freed (HeapTree_Remove). A shared sentinel,
// HeapTree_NilNode, ends every leaf, and has level 0 and links to itself, which is what lets skew and split look
// two levels down without testing for the end of the tree.
//
// tests/heaptree_difftest.c runs this against the original implementation (the same random inserts and removes
// on both, comparing the shape of the tree after each) under ASan and UBSan.

#include <stdlib.h>
#include <stdint.h>
#include "HeapTree.h"

// (a tree of this many levels would be 2^40 objects)
#define MAX_TREE_DEPTH 40

tHeapNode *HeapTree_NilNode;
tHeapNode *HeapTree_RootNode;

#define nil HeapTree_NilNode

void HeapTree_Init(void) {
	nil = (tHeapNode*)calloc(1, sizeof(tHeapNode));
	nil->pLink[0] = nil;
	nil->pLink[1] = nil;
	HeapTree_RootNode = nil;
}

// Left rotation that removes a horizontal link going left: a node and its left child on the same level
static tHeapNode* Skew(tHeapNode *pRoot) {
	if (pRoot->pLink[0]->level == pRoot->level && pRoot->level != 0) {
		tHeapNode *pSave = pRoot->pLink[0];
		pRoot->pLink[0] = pSave->pLink[1];
		pSave->pLink[1] = pRoot;
		pRoot = pSave;
	}
	return pRoot;
}

// Right rotation that removes two horizontal links going right in a row: a node, its right child and that
// child's right child, all on the same level
static tHeapNode* Split(tHeapNode *pRoot) {
	if (pRoot->pLink[1]->pLink[1]->level == pRoot->level && pRoot->level != 0) {
		tHeapNode *pSave = pRoot->pLink[1];
		pRoot->pLink[1] = pSave->pLink[0];
		pSave->pLink[0] = pRoot;
		pRoot = pSave;
		pRoot->level++;
	}
	return pRoot;
}

void HeapTree_Insert(tHeapNode *pEntry) {
	pEntry->level = 1;
	pEntry->pLink[0] = nil;
	pEntry->pLink[1] = nil;
	pEntry->marked = 0;

	if (HeapTree_RootNode == nil) {
		HeapTree_RootNode = pEntry;
	} else {
		tHeapNode *pNode = HeapTree_RootNode;
		tHeapNode *pUp[MAX_TREE_DEPTH];
		int top = 0, dir;

		// Find the leaf position to insert at, remembering the path. This first step does not balance
		for (;;) {
			pUp[top++] = pNode;
			dir = (uintptr_t)pNode < (uintptr_t)pEntry;         // 0 for left, 1 for right
			if (pNode->pLink[dir] == nil) {
				break;
			}
			pNode = pNode->pLink[dir];
		}
		pNode->pLink[dir] = pEntry;

		// Walk back up the path, rebalancing, and link what each step returns into its parent
		for (top--; top >= 0; top--) {
			if (top != 0) {
				dir = pUp[top - 1]->pLink[1] == pUp[top];
			}
			pUp[top] = Skew(pUp[top]);
			pUp[top] = Split(pUp[top]);
			if (top != 0) {
				pUp[top - 1]->pLink[dir] = pUp[top];
			} else {
				HeapTree_RootNode = pUp[0];
			}
		}
	}
}

// Remove pDelete from the subtree at pRoot (recursively, so no deeper than the tree is), returning the new root of it
static tHeapNode* RemoveAt(tHeapNode *pRoot, tHeapNode *pDelete) {
	if (pRoot != nil) {
		if (pRoot == pDelete) {
			if (pRoot->pLink[0] != nil && pRoot->pLink[1] != nil) {
				// Two children: the node is replaced by its heir, the greatest node of its left subtree, and removed from where
				// that was, which has at most one child
				tHeapNode *pHeir = pRoot->pLink[0];
				tHeapNode **ppHeirLink = &pHeir->pLink[0];
				tHeapNode *pL0;
				unsigned char l;
				while (pHeir->pLink[1] != nil) {
					ppHeirLink = &pHeir->pLink[1];
					pHeir = pHeir->pLink[1];
				}
				// Swap the two nodes' places in the tree
				pL0 = pHeir->pLink[0];
				l = pHeir->level;
				pHeir->pLink[0] = pRoot->pLink[0];
				pHeir->pLink[1] = pRoot->pLink[1];
				pHeir->level = pRoot->level;
				*ppHeirLink = pRoot;
				pRoot->pLink[0] = pL0;
				pRoot->pLink[1] = nil;
				pRoot->level = l;
				// The heir is the root of this subtree now, and the node is deleted from further down
				pL0 = pRoot;
				pRoot = pHeir;
				pRoot->pLink[0] = RemoveAt(pRoot->pLink[0], pL0);
			} else {
				pRoot = pRoot->pLink[pRoot->pLink[0] == nil];
			}
		} else {
			int dir = (uintptr_t)pRoot < (uintptr_t)pDelete;
			pRoot->pLink[dir] = RemoveAt(pRoot->pLink[dir], pDelete);
		}
	}

	// Rebalance on the way back up. (At the sentinel, level 0, nothing below is shorter than it, so this does nothing)
	if (pRoot->pLink[0]->level < pRoot->level - 1 || pRoot->pLink[1]->level < pRoot->level - 1) {
		pRoot->level--;
		if (pRoot->pLink[1]->level > pRoot->level) {
			pRoot->pLink[1]->level = pRoot->level;
		}
		pRoot = Skew(pRoot);
		pRoot->pLink[1] = Skew(pRoot->pLink[1]);
		pRoot->pLink[1]->pLink[1] = Skew(pRoot->pLink[1]->pLink[1]);
		pRoot = Split(pRoot);
		pRoot->pLink[1] = Split(pRoot->pLink[1]);
	}

	return pRoot;
}

void HeapTree_Remove(tHeapNode *pEntry) {
	HeapTree_RootNode = RemoveAt(HeapTree_RootNode, pEntry);
}
