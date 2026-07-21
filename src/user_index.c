/*
 * user_index.c — Offset-based Red-Black Tree in shared memory (v1.4)
 *
 * All node pointers are int32_t byte-offsets from the mmap base.
 * Since all processes share the mmap at the same virtual address
 * (inherited via fork), TREE(off) returns a valid C pointer.
 */

#include "../include/user_index.h"
#include "../include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- globals (from user_store.c) ---- */
extern void *g_shm_base;
extern BSTnode *g_nil;

/* ---- helpers ---- */
#define LIST(off)  ((off) ? (ListNode *)((char *)g_shm_base + (off)) : NULL)
#define TREE(off)  ((off) ? (BSTnode *)((char *)g_shm_base + (off)) : NULL)

#define OFF_L(p)   ((p) ? (int32_t)((char *)(p) - (char *)g_shm_base) : 0)
#define OFF_T(p)   ((p) ? (int32_t)((char *)(p) - (char *)g_shm_base) : 0)

#define IS_NIL(n)  ((n) == g_nil)

/* ================================================================
 * bst_init
 * ================================================================ */
void bst_init(BST *tree) {
    if (!tree) return;
    tree->nil_off  = OFF_T(g_nil);
    tree->root_off = OFF_T(g_nil);
    tree->size = 0;
}

/* ================================================================
 * Rotations
 * ================================================================ */
void rbt_left_rotate(BST *tree, BSTnode *x) {
    BSTnode *y = TREE(x->right_off);
    x->right_off = y->left_off;
    if (!IS_NIL(TREE(y->left_off)))
        TREE(y->left_off)->parent_off = OFF_T(x);
    y->parent_off = x->parent_off;
    if (IS_NIL(TREE(x->parent_off)))
        tree->root_off = OFF_T(y);
    else if (x == TREE(TREE(x->parent_off)->left_off))
        TREE(x->parent_off)->left_off = OFF_T(y);
    else
        TREE(x->parent_off)->right_off = OFF_T(y);
    y->left_off = OFF_T(x);
    x->parent_off = OFF_T(y);
}

void rbt_right_rotate(BST *tree, BSTnode *x) {
    BSTnode *y = TREE(x->left_off);
    x->left_off = y->right_off;
    if (!IS_NIL(TREE(y->right_off)))
        TREE(y->right_off)->parent_off = OFF_T(x);
    y->parent_off = x->parent_off;
    if (IS_NIL(TREE(x->parent_off)))
        tree->root_off = OFF_T(y);
    else if (x == TREE(TREE(x->parent_off)->right_off))
        TREE(x->parent_off)->right_off = OFF_T(y);
    else
        TREE(x->parent_off)->left_off = OFF_T(y);
    y->right_off = OFF_T(x);
    x->parent_off = OFF_T(y);
}

/* ================================================================
 * bst_insert
 * ================================================================ */
int bst_insert(BST *tree, ListNode *user) {
    BSTnode *z, *y, *x;

    if (!tree || !user) return -1;

    /* Find the BSTnode that contains this user's offset */
    z = NULL;
    {
        /* search for a tree node whose user_off matches */
        /* Actually, the caller already created a BSTnode and set user_off.
         * We need to find it. Since it was just allocated and has user_off set,
         * we can find it by scanning. But better: we assume the caller
         * passes a ListNode, and we find the BSTnode by searching.
         *
         * Actually, simpler: the caller already allocates BSTnode and sets
         * user_off. We just insert that node into the tree. We find z by
         * looking up the BSTnode that was just allocated.
         *
         * Even simpler: the caller should pass the BSTnode too. But the API
         * only takes ListNode. Let's find the BSTnode by scanning from
         * tree_next_free-1 backwards.
         *
         * SIMPLEST: the BSTnode was just allocated at tree_next_free-1.
         * We'll locate it by index.
         */
        extern BSTnode *g_tree_pool;
        extern shm_header_t *g_header;
        int32_t idx = g_header->tree_next_free - 1;
        z = &g_tree_pool[idx];
    }

    z->left_off  = OFF_T(g_nil);
    z->right_off = OFF_T(g_nil);
    z->color = RBT_RED;

    y = g_nil;
    x = TREE(tree->root_off);

    while (!IS_NIL(x)) {
        y = x;
        ListNode *xu = LIST(x->user_off);
        if (xu && strcmp(user->data.name, xu->data.name) < 0)
            x = TREE(x->left_off);
        else
            x = TREE(x->right_off);
    }

    z->parent_off = OFF_T(y);
    if (IS_NIL(y))
        tree->root_off = OFF_T(z);
    else {
        ListNode *yu = LIST(y->user_off);
        if (yu && strcmp(user->data.name, yu->data.name) < 0)
            y->left_off = OFF_T(z);
        else
            y->right_off = OFF_T(z);
    }

    tree->size++;
    rbt_insert_fixup(tree, z);
    return 0;
}

void rbt_insert_fixup(BST *tree, BSTnode *z) {
    while (TREE(z->parent_off)->color == RBT_RED) {
        BSTnode *zp = TREE(z->parent_off);
        BSTnode *zpp = TREE(zp->parent_off);
        if (zp == TREE(zpp->left_off)) {
            BSTnode *y = TREE(zpp->right_off);
            if (y->color == RBT_RED) {
                zp->color = RBT_BLACK;
                y->color = RBT_BLACK;
                zpp->color = RBT_RED;
                z = zpp;
            } else {
                if (z == TREE(zp->right_off)) {
                    z = zp;
                    rbt_left_rotate(tree, z);
                    zp = TREE(z->parent_off);
                    zpp = TREE(zp->parent_off);
                }
                zp->color = RBT_BLACK;
                zpp->color = RBT_RED;
                rbt_right_rotate(tree, zpp);
            }
        } else {
            BSTnode *y = TREE(zpp->left_off);
            if (y->color == RBT_RED) {
                zp->color = RBT_BLACK;
                y->color = RBT_BLACK;
                zpp->color = RBT_RED;
                z = zpp;
            } else {
                if (z == TREE(zp->left_off)) {
                    z = zp;
                    rbt_right_rotate(tree, z);
                    zp = TREE(z->parent_off);
                    zpp = TREE(zp->parent_off);
                }
                zp->color = RBT_BLACK;
                zpp->color = RBT_RED;
                rbt_left_rotate(tree, zpp);
            }
        }
    }
    TREE(tree->root_off)->color = RBT_BLACK;
}

/* ================================================================
 * bst_find
 * ================================================================ */
ListNode *bst_find(BST *tree, const char *name) {
    BSTnode *x;
    if (!tree || !name || !g_shm_base) return NULL;
    x = TREE(tree->root_off);
    while (!IS_NIL(x)) {
        ListNode *xu = LIST(x->user_off);
        if (!xu) break;
        int cmp = strcmp(name, xu->data.name);
        if (cmp == 0) return xu;
        x = TREE(cmp < 0 ? x->left_off : x->right_off);
    }
    return NULL;
}

ListNode *bst_find_with_steps(BST *tree, const char *name, int *steps, int verbose) {
    BSTnode *x;
    int s = 0;
    if (!tree || !name || !g_shm_base) return NULL;
    x = TREE(tree->root_off);
    while (!IS_NIL(x)) {
        s++;
        ListNode *xu = LIST(x->user_off);
        if (!xu) break;
        if (verbose) {
            printf("  [BST step %d] checking: %s\n", s, xu->data.name);
        }
        int cmp = strcmp(name, xu->data.name);
        if (cmp == 0) { if (steps) *steps = s; return xu; }
        x = TREE(cmp < 0 ? x->left_off : x->right_off);
    }
    if (steps) *steps = s;
    return NULL;
}

/* ================================================================
 * bst_delete
 * ================================================================ */
BSTnode *rbt_minimum(BST *tree, BSTnode *node) {
    while (!IS_NIL(TREE(node->left_off)))
        node = TREE(node->left_off);
    return node;
}

void rbt_transplant(BST *tree, BSTnode *u, BSTnode *v) {
    if (IS_NIL(TREE(u->parent_off)))
        tree->root_off = OFF_T(v);
    else if (u == TREE(TREE(u->parent_off)->left_off))
        TREE(u->parent_off)->left_off = OFF_T(v);
    else
        TREE(u->parent_off)->right_off = OFF_T(v);
    v->parent_off = u->parent_off;
}

int bst_delete(BST *tree, const char *name) {
    BSTnode *z, *x, *y;
    int y_orig_color;

    /* find the node to delete */
    z = TREE(tree->root_off);
    while (!IS_NIL(z)) {
        ListNode *zu = LIST(z->user_off);
        if (!zu) break;
        int cmp = strcmp(name, zu->data.name);
        if (cmp == 0) break;
        z = TREE(cmp < 0 ? z->left_off : z->right_off);
    }
    if (IS_NIL(z)) return -1;

    y = z;
    y_orig_color = y->color;

    if (IS_NIL(TREE(z->left_off))) {
        x = TREE(z->right_off);
        rbt_transplant(tree, z, x);
    } else if (IS_NIL(TREE(z->right_off))) {
        x = TREE(z->left_off);
        rbt_transplant(tree, z, x);
    } else {
        y = rbt_minimum(tree, TREE(z->right_off));
        y_orig_color = y->color;
        x = TREE(y->right_off);
        if (TREE(y->parent_off) == z) {
            x->parent_off = OFF_T(y);
        } else {
            rbt_transplant(tree, y, x);
            y->right_off = z->right_off;
            TREE(y->right_off)->parent_off = OFF_T(y);
        }
        rbt_transplant(tree, z, y);
        y->left_off = z->left_off;
        TREE(y->left_off)->parent_off = OFF_T(y);
        y->color = z->color;
    }

    /* mark the deleted BSTnode as unused */
    z->used = 0;

    if (y_orig_color == RBT_BLACK)
        rbt_delete_fixup(tree, x);

    tree->size--;
    return 0;
}

void rbt_delete_fixup(BST *tree, BSTnode *x) {
    while (x != TREE(tree->root_off) && x->color == RBT_BLACK) {
        BSTnode *xp = TREE(x->parent_off);
        if (x == TREE(xp->left_off)) {
            BSTnode *w = TREE(xp->right_off);
            if (w->color == RBT_RED) {
                w->color = RBT_BLACK;
                xp->color = RBT_RED;
                rbt_left_rotate(tree, xp);
                xp = TREE(x->parent_off);
                w = TREE(xp->right_off);
            }
            if (TREE(w->left_off)->color == RBT_BLACK &&
                TREE(w->right_off)->color == RBT_BLACK) {
                w->color = RBT_RED;
                x = xp;
            } else {
                if (TREE(w->right_off)->color == RBT_BLACK) {
                    TREE(w->left_off)->color = RBT_BLACK;
                    w->color = RBT_RED;
                    rbt_right_rotate(tree, w);
                    xp = TREE(x->parent_off);
                    w = TREE(xp->right_off);
                }
                w->color = xp->color;
                xp->color = RBT_BLACK;
                TREE(w->right_off)->color = RBT_BLACK;
                rbt_left_rotate(tree, xp);
                x = TREE(tree->root_off);
            }
        } else {
            BSTnode *w = TREE(xp->left_off);
            if (w->color == RBT_RED) {
                w->color = RBT_BLACK;
                xp->color = RBT_RED;
                rbt_right_rotate(tree, xp);
                xp = TREE(x->parent_off);
                w = TREE(xp->left_off);
            }
            if (TREE(w->right_off)->color == RBT_BLACK &&
                TREE(w->left_off)->color == RBT_BLACK) {
                w->color = RBT_RED;
                x = xp;
            } else {
                if (TREE(w->left_off)->color == RBT_BLACK) {
                    TREE(w->right_off)->color = RBT_BLACK;
                    w->color = RBT_RED;
                    rbt_left_rotate(tree, w);
                    xp = TREE(x->parent_off);
                    w = TREE(xp->left_off);
                }
                w->color = xp->color;
                xp->color = RBT_BLACK;
                TREE(w->left_off)->color = RBT_BLACK;
                rbt_right_rotate(tree, xp);
                x = TREE(tree->root_off);
            }
        }
    }
    x->color = RBT_BLACK;
}

void rbt_find_predecessor(BST *tree, BSTnode *node,
                           BSTnode **out_pre, BSTnode **out_parent) {
    BSTnode *parent = g_nil;
    BSTnode *x = TREE(tree->root_off);
    BSTnode *pre = g_nil;

    while (!IS_NIL(x)) {
        ListNode *xu = LIST(x->user_off);
        ListNode *nu = LIST(node->user_off);
        if (!xu || !nu) break;
        parent = x;
        if (strcmp(nu->data.name, xu->data.name) <= 0) {
            x = TREE(x->left_off);
        } else {
            pre = x;
            x = TREE(x->right_off);
        }
    }
    if (out_pre) *out_pre = pre;
    if (out_parent) *out_parent = parent;
}

/* ================================================================
 * Traversal
 * ================================================================ */
static void bst_inorder_node(BSTnode *node, BSTnode *nil) {
    if (IS_NIL(node)) return;
    bst_inorder_node(TREE(node->left_off), nil);
    if (node->used && node->user_off) {
        ListNode *lu = LIST(node->user_off);
        if (lu && lu->used) {
            printf("%s %s %s %s %s %s\n",
                   lu->data.name, lu->data.password, lu->data.birthdate,
                   lu->data.phone, lu->data.mobile, lu->data.email);
        }
    }
    bst_inorder_node(TREE(node->right_off), nil);
}

void bst_inorder(BST *tree) {
    if (!tree || !g_shm_base) return;
    bst_inorder_node(TREE(tree->root_off), TREE(tree->nil_off));
}

/* ---- format users to buffer ---- */
static void bst_format_node(BSTnode *node, BSTnode *nil,
                             char *buf, int buf_size,
                             int *total, int *offset) {
    if (IS_NIL(node) || *offset >= buf_size - 200) return;
    bst_format_node(TREE(node->left_off), nil, buf, buf_size, total, offset);
    if (node->used && node->user_off) {
        ListNode *lu = LIST(node->user_off);
        if (lu && lu->used) {
            (*total)++;
            int w = snprintf(buf + *offset, buf_size - *offset,
                     "%s %s %s %s %s %s\n",
                     lu->data.name, lu->data.password, lu->data.birthdate,
                     lu->data.phone, lu->data.mobile, lu->data.email);
            if (w > 0 && w < buf_size - *offset) *offset += w;
        }
    }
    bst_format_node(TREE(node->right_off), nil, buf, buf_size, total, offset);
}

void bst_format_users(BST *tree, char *buf, int buf_size, int *total, int *offset) {
    if (!tree || !buf || !g_shm_base) return;
    bst_format_node(TREE(tree->root_off), TREE(tree->nil_off),
                    buf, buf_size, total, offset);
}

/* ---- free (no-op: memory is in mmap pool) ---- */
void bst_free(BST *tree) {
    if (!tree) return;
    tree->root_off = tree->nil_off;
    tree->size = 0;
}
