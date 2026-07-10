#ifndef USER_INDEX_H
#define USER_INDEX_H

#include "user_store.h"

/* Red-Black Tree color constants */
#define RBT_RED   0
#define RBT_BLACK 1

typedef struct TreeNode {
    ListPtr user;
    struct TreeNode *left;
    struct TreeNode *right;
    struct TreeNode *parent;
    int color;
} BSTnode;

typedef struct {
    BSTnode *root;
    BSTnode nil;      /* sentinel NIL node, always BLACK */
    int size;
} BST;

void bst_init(BST *tree);
int bst_insert(BST *tree, ListPtr user);
ListPtr bst_find(BST *tree, const char *name);
ListPtr bst_find_with_steps(BST *tree, const char *name, int *steps, int verbose);
int bst_delete(BST *tree, const char *name);
void bst_inorder(BST *tree);
void bst_free(BST *tree);

/* Internal red-black tree helpers */
void rbt_left_rotate(BST *tree, BSTnode *x);
void rbt_right_rotate(BST *tree, BSTnode *x);
void rbt_insert_fixup(BST *tree, BSTnode *z);
void rbt_transplant(BST *tree, BSTnode *u, BSTnode *v);
BSTnode *rbt_minimum(BST *tree, BSTnode *node);
void rbt_find_predecessor(BST *tree, BSTnode *node,
                           BSTnode **out_pre, BSTnode **out_parent);
void rbt_delete_fixup(BST *tree, BSTnode *x);

#endif
