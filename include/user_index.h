#ifndef USER_INDEX_H
#define USER_INDEX_H

#include "user_store.h"

/*
 * v1.4: Red-Black Tree using offset-based pointers in shared memory.
 * All node pointers are int32_t byte-offsets from the mmap base.
 */

/* RBT color constants */
#define RBT_RED   0
#define RBT_BLACK 1

/* Legacy BST struct (kept for API compatibility, contents are offset-based) */
typedef struct {
    int32_t root_off;     /* offset to root BSTnode */
    int32_t nil_off;      /* offset to NIL sentinel */
    int32_t size;
} BST;

/* ---- API ---- */
void bst_init(BST *tree);
int  bst_insert(BST *tree, ListNode *user);
ListNode *bst_find(BST *tree, const char *name);
ListNode *bst_find_with_steps(BST *tree, const char *name, int *steps, int verbose);
int  bst_delete(BST *tree, const char *name);
void bst_inorder(BST *tree);
void bst_free(BST *tree);
void bst_format_users(BST *tree, char *buf, int buf_size, int *total, int *offset);

/* Internal helpers (exposed for user_store.c) */
void rbt_left_rotate(BST *tree, BSTnode *x);
void rbt_right_rotate(BST *tree, BSTnode *x);
void rbt_insert_fixup(BST *tree, BSTnode *z);
void rbt_transplant(BST *tree, BSTnode *u, BSTnode *v);
BSTnode *rbt_minimum(BST *tree, BSTnode *node);
void rbt_find_predecessor(BST *tree, BSTnode *node,
                           BSTnode **out_pre, BSTnode **out_parent);
void rbt_delete_fixup(BST *tree, BSTnode *x);

#endif
