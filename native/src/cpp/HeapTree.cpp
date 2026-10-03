// Andersson (AA) tree over heap entries, ordered by memory address.
// See http://www.eternallyconfuzzled.com/tuts/datastructures/jsw_tut_andersson.aspx
//
// Written in the Crust C++ subset: no exceptions, no RTTI, no STL, no static
// constructors. Lowered to C by tools/cpprust.py.

#include <stdlib.h>
#include <string.h>
#include "../HeapTree.h"

#define MAX_TREE_DEPTH 40

class AATree {
    tHeapEntry *root;
    tHeapEntry *nil;

public:
    void init() {
        nil = (tHeapEntry *)calloc(1, sizeof(tHeapEntry));
        nil->pLink[0] = nil;
        nil->pLink[1] = nil;
        root = nil;
    }

    tHeapEntry *get_root() { return root; }
    tHeapEntry *get_nil() { return nil; }

    void insert(tHeapEntry *pEntry) {
        root = insert_at(root, pEntry);
    }

    void remove(tHeapEntry *pEntry) {
        root = remove_at(root, pEntry);
    }

private:
    tHeapEntry *skew(tHeapEntry *pRoot) {
        if (pRoot->pLink[0]->level == pRoot->level && pRoot->level != 0) {
            tHeapEntry *pSave = pRoot->pLink[0];
            pRoot->pLink[0] = pSave->pLink[1];
            pSave->pLink[1] = pRoot;
            pRoot = pSave;
        }
        return pRoot;
    }

    tHeapEntry *split(tHeapEntry *pRoot) {
        if (pRoot->pLink[1]->pLink[1]->level == pRoot->level && pRoot->level != 0) {
            tHeapEntry *pSave = pRoot->pLink[1];
            pRoot->pLink[1] = pSave->pLink[0];
            pSave->pLink[0] = pRoot;
            pRoot = pSave;
            pRoot->level++;
        }
        return pRoot;
    }

    tHeapEntry *insert_at(tHeapEntry *pRoot, tHeapEntry *pEntry) {
        if (pRoot == nil) {
            pRoot = pEntry;
            pRoot->level = 1;
            pRoot->pLink[0] = nil;
            pRoot->pLink[1] = nil;
            pRoot->marked = 0;
        } else {
            tHeapEntry *pNode = root;
            tHeapEntry *pUp[MAX_TREE_DEPTH];
            int top = 0;
            int dir;
            // Find leaf position to insert into tree. This first step is unbalanced
            for (;;) {
                pUp[top] = pNode;
                top++;
                dir = pNode < pEntry; // 0 for left, 1 for right
                if (pNode->pLink[dir] == nil) {
                    break;
                }
                pNode = pNode->pLink[dir];
            }
            // Create new node
            pNode->pLink[dir] = pEntry;
            pEntry->level = 1;
            pEntry->pLink[0] = nil;
            pEntry->pLink[1] = nil;
            pEntry->marked = 0;
            // Balance the tree
            top--;
            while (top >= 0) {
                if (top != 0) {
                    dir = pUp[top - 1]->pLink[1] == pUp[top];
                }
                pUp[top] = skew(pUp[top]);
                pUp[top] = split(pUp[top]);
                if (top != 0) {
                    pUp[top - 1]->pLink[dir] = pUp[top];
                } else {
                    pRoot = pUp[0];
                }
                top--;
            }
        }
        return pRoot;
    }

    tHeapEntry *remove_at(tHeapEntry *pRoot, tHeapEntry *pDelete) {
        if (pRoot != nil) {
            if (pRoot == pDelete) {
                if (pRoot->pLink[0] != nil && pRoot->pLink[1] != nil) {
                    tHeapEntry *pL0;
                    unsigned char l;
                    tHeapEntry *pHeir = pRoot->pLink[0];
                    tHeapEntry **ppHeirLink = &pHeir->pLink[0];
                    while (pHeir->pLink[1] != nil) {
                        ppHeirLink = &pHeir->pLink[1];
                        pHeir = pHeir->pLink[1];
                    }
                    // Swap the two nodes
                    pL0 = pHeir->pLink[0];
                    l = pHeir->level;
                    // Bring heir to replace root
                    pHeir->pLink[0] = pRoot->pLink[0];
                    pHeir->pLink[1] = pRoot->pLink[1];
                    pHeir->level = pRoot->level;
                    // Send root to replace heir
                    *ppHeirLink = pRoot;
                    pRoot->pLink[0] = pL0;
                    pRoot->pLink[1] = nil;
                    pRoot->level = l;
                    // Set correct return value
                    pL0 = pRoot;
                    pRoot = pHeir;
                    // Delete the node that's been sent down
                    pRoot->pLink[0] = remove_at(pRoot->pLink[0], pL0);
                } else {
                    pRoot = pRoot->pLink[pRoot->pLink[0] == nil];
                }
            } else {
                int dir = pRoot < pDelete;
                pRoot->pLink[dir] = remove_at(pRoot->pLink[dir], pDelete);
            }
        }

        if (pRoot->pLink[0]->level < pRoot->level - 1 || pRoot->pLink[1]->level < pRoot->level - 1) {
            pRoot->level--;
            if (pRoot->pLink[1]->level > pRoot->level) {
                pRoot->pLink[1]->level = pRoot->level;
            }
            pRoot = skew(pRoot);
            pRoot->pLink[1] = skew(pRoot->pLink[1]);
            pRoot->pLink[1]->pLink[1] = skew(pRoot->pLink[1]->pLink[1]);
            pRoot = split(pRoot);
            pRoot->pLink[1] = split(pRoot->pLink[1]);
        }

        return pRoot;
    }
};

// ---- C API (see HeapTree.h) ----
// No static constructors in the subset, so the single instance is zeroed
// storage that HeapTree_Init() brings up.
static AATree g_tree;

tHeapEntry *HeapTree_NilNode;

void HeapTree_Init(void) {
    g_tree.init();
    HeapTree_NilNode = g_tree.get_nil();
}
tHeapEntry *HeapTree_Root(void) { return g_tree.get_root(); }
void HeapTree_Insert(tHeapEntry *pEntry) { g_tree.insert(pEntry); }
void HeapTree_Remove(tHeapEntry *pEntry) { g_tree.remove(pEntry); }
