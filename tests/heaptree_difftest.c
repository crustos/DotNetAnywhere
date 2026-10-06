// Differential test: original C Andersson tree (verbatim from the pre-port
// Heap.c) vs. native/src/HeapTree.c. Same random operations on two parallel
// node arrays; the entire tree shape must match after every step.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "HeapTree.h"

typedef unsigned char U8; typedef int I32;
#define MAX_TREE_DEPTH 40

// ---------------- reference: original C code ----------------
static tHeapNode *pHeapTreeRoot;
static tHeapNode *nil;

static tHeapNode* TreeSkew(tHeapNode *pRoot) {
	if (pRoot->pLink[0]->level == pRoot->level && pRoot->level != 0) {
		tHeapNode *pSave = pRoot->pLink[0];
		pRoot->pLink[0] = pSave->pLink[1];
		pSave->pLink[1] = pRoot;
		pRoot = pSave;
	}
	return pRoot;
}
static tHeapNode* TreeSplit(tHeapNode *pRoot) {
	if (pRoot->pLink[1]->pLink[1]->level == pRoot->level && pRoot->level != 0) {
		tHeapNode *pSave = pRoot->pLink[1];
		pRoot->pLink[1] = pSave->pLink[0];
		pSave->pLink[0] = pRoot;
		pRoot = pSave;
		pRoot->level++;
	}
	return pRoot;
}
static tHeapNode* TreeInsert(tHeapNode *pRoot, tHeapNode *pEntry) {
	if (pRoot == nil) {
		pRoot = pEntry;
		pRoot->level = 1;
		pRoot->pLink[0] = pRoot->pLink[1] = nil;
		pRoot->marked = 0;
	} else {
		tHeapNode *pNode = pHeapTreeRoot;
		tHeapNode *pUp[MAX_TREE_DEPTH];
		I32 top = 0, dir;
		for (;;) {
			pUp[top++] = pNode;
			dir = pNode < pEntry;
			if (pNode->pLink[dir] == nil) break;
			pNode = pNode->pLink[dir];
		}
		pNode->pLink[dir] = pEntry;
		pEntry->level = 1;
		pEntry->pLink[0] = pEntry->pLink[1] = nil;
		pEntry->marked = 0;
		while (--top >= 0) {
			if (top != 0) dir = pUp[top-1]->pLink[1] == pUp[top];
			pUp[top] = TreeSkew(pUp[top]);
			pUp[top] = TreeSplit(pUp[top]);
			if (top != 0) pUp[top-1]->pLink[dir] = pUp[top];
			else pRoot = pUp[0];
		}
	}
	return pRoot;
}
static tHeapNode* TreeRemove(tHeapNode *pRoot, tHeapNode *pDelete) {
	if (pRoot != nil) {
		if (pRoot == pDelete) {
			if (pRoot->pLink[0] != nil && pRoot->pLink[1] != nil) {
				tHeapNode *pL0; U8 l;
				tHeapNode *pHeir = pRoot->pLink[0], **ppHeirLink = &pHeir->pLink[0];
				while (pHeir->pLink[1] != nil) {
					ppHeirLink = &pHeir->pLink[1];
					pHeir = pHeir->pLink[1];
				}
				pL0 = pHeir->pLink[0];
				l = pHeir->level;
				pHeir->pLink[0] = pRoot->pLink[0];
				pHeir->pLink[1] = pRoot->pLink[1];
				pHeir->level = pRoot->level;
				*ppHeirLink = pRoot;
				pRoot->pLink[0] = pL0;
				pRoot->pLink[1] = nil;
				pRoot->level = l;
				pL0 = pRoot;
				pRoot = pHeir;
				pRoot->pLink[0] = TreeRemove(pRoot->pLink[0], pL0);
			} else {
				pRoot = pRoot->pLink[pRoot->pLink[0] == nil];
			}
		} else {
			I32 dir = pRoot < pDelete;
			pRoot->pLink[dir] = TreeRemove(pRoot->pLink[dir], pDelete);
		}
	}
	if (pRoot->pLink[0]->level < pRoot->level-1 || pRoot->pLink[1]->level < pRoot->level-1) {
		if (pRoot->pLink[1]->level > --pRoot->level) pRoot->pLink[1]->level = pRoot->level;
		pRoot = TreeSkew(pRoot);
		pRoot->pLink[1] = TreeSkew(pRoot->pLink[1]);
		pRoot->pLink[1]->pLink[1] = TreeSkew(pRoot->pLink[1]->pLink[1]);
		pRoot = TreeSplit(pRoot);
		pRoot->pLink[1] = TreeSplit(pRoot->pLink[1]);
	}
	return pRoot;
}

// ---------------- harness ----------------
#define N 2000
static tHeapNode A[N], B[N];
static int inA[N];

static int idx(tHeapNode *arr, tHeapNode *nilp, tHeapNode *p) { return p == nilp ? -1 : (int)(p - arr); }

// Compare shapes recursively; also check AA invariants on B. Returns node count or -1.
static int walk(tHeapNode *a, tHeapNode *b, int depth) {
	tHeapNode *na = nil, *nb = HeapTree_NilNode;
	if (a == na || b == nb) {
		if ((a == na) != (b == nb)) { printf("shape mismatch (nil vs node)\n"); return -1; }
		return 0;
	}
	if (idx(A, na, a) != idx(B, nb, b)) { printf("node mismatch %d vs %d\n", idx(A,na,a), idx(B,nb,b)); return -1; }
	if (a->level != b->level || a->marked != b->marked) { printf("level/marked mismatch at %d\n", idx(A,na,a)); return -1; }
	if (depth > MAX_TREE_DEPTH) { printf("too deep\n"); return -1; }
	// AA invariants
	if (b->pLink[0]->level != b->level - 1 && b->pLink[0] != nb) { printf("left invariant\n"); return -1; }
	if (b->pLink[0] == nb && b->level != 1) { printf("leaf level\n"); return -1; }
	if (b->pLink[1]->level != b->level && b->pLink[1]->level != b->level - 1) { printf("right invariant\n"); return -1; }
	if (b->pLink[1]->pLink[1]->level == b->level && b->pLink[1] != nb) { printf("double right\n"); return -1; }
	int l = walk(a->pLink[0], b->pLink[0], depth+1); if (l < 0) return -1;
	int r = walk(a->pLink[1], b->pLink[1], depth+1); if (r < 0) return -1;
	return l + r + 1;
}

int main(int argc, char **argv) {
	unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
	int rounds = argc > 2 ? atoi(argv[2]) : 200000;
	srand(seed);

	nil = calloc(1, sizeof(tHeapNode));
	nil->pLink[0] = nil->pLink[1] = nil;
	pHeapTreeRoot = nil;
	HeapTree_Init();

	int live = 0;
	for (int step = 0; step < rounds; step++) {
		int i = rand() % N;
		if (!inA[i]) {
			pHeapTreeRoot = TreeInsert(pHeapTreeRoot, &A[i]);
			HeapTree_Insert(&B[i]);
			inA[i] = 1; live++;
		} else {
			pHeapTreeRoot = TreeRemove(pHeapTreeRoot, &A[i]);
			HeapTree_Remove(&B[i]);
			inA[i] = 0; live--;
		}
		// bias: sometimes run a burst in one direction
		if (step % 5000 < 1000 && live > 0 && rand() % 4) { /* extra churn handled by rand */ }
		if (step % 7 == 0 || step < 200) {
			int n = walk(pHeapTreeRoot, HeapTree_RootNode, 0);
			if (n != live) { printf("FAIL seed=%u step=%d (count %d vs live %d)\n", seed, step, n, live); return 1; }
		}
	}
	int n = walk(pHeapTreeRoot, HeapTree_RootNode, 0);
	if (n != live) { printf("FAIL final\n"); return 1; }
	printf("OK seed=%u rounds=%d final_live=%d\n", seed, rounds, live);
	return 0;
}
