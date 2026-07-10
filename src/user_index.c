#include "../include/user_index.h"
#include "../include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * bst_init
 * ================================================================ */
void bst_init(BST *tree) {
    if (tree == NULL) {
        return;
    }

    /* sentinel NIL: always BLACK, self-looping pointers for safety */
    tree->nil.color  = RBT_BLACK;
    tree->nil.left   = &tree->nil;
    tree->nil.right  = &tree->nil;
    tree->nil.parent = &tree->nil;
    tree->nil.user   = NULL;

    tree->root = &tree->nil;
    tree->size = 0;
}

/* ================================================================
 * Rotations (preserve BST property, adjust RBT colors later)
 * ================================================================ */

/*
 * left_rotate:     x                 y
 *                 / \      =>       / \
 *                a   y             x   c
 *                   / \           / \
 *                  b   c         a   b
 */
void rbt_left_rotate(BST *tree, BSTnode *x) {
    BSTnode *y = x->right;

    /* turn y's left subtree into x's right subtree */
    x->right = y->left;
    if (y->left != &tree->nil) {
        y->left->parent = x;
    }

    /* link y to x's parent */
    y->parent = x->parent;
    if (x->parent == &tree->nil) {
        tree->root = y;               /* x was root */
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }

    /* put x on y's left */
    y->left = x;
    x->parent = y;
}

/*
 * right_rotate:       x              y
 *                    / \     =>     / \
 *                   y   a          b   x
 *                  / \                / \
 *                 b   c              c   a
 */
void rbt_right_rotate(BST *tree, BSTnode *x) {
    BSTnode *y = x->left;

    /* turn y's right subtree into x's left subtree */
    x->left = y->right;
    if (y->right != &tree->nil) {
        y->right->parent = x;
    }

    /* link y to x's parent */
    y->parent = x->parent;
    if (x->parent == &tree->nil) {
        tree->root = y;               /* x was root */
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }

    /* put x on y's right */
    y->right = x;
    x->parent = y;
}

/* ================================================================
 * bst_insert (Red-Black Tree insert with fixup)
 * ================================================================ */
int bst_insert(BST *tree, ListPtr user) {
    BSTnode *new_node;
    BSTnode *current;
    BSTnode *parent;

    if (tree == NULL || user == NULL) {
        log_error("rbt: insert called with NULL tree or user");
        return -1;
    }

    /* allocate new RED node */
    new_node = (BSTnode *)malloc(sizeof(BSTnode));
    if (new_node == NULL) {
        log_error("rbt: malloc failed during insert");
        return -1;
    }

    new_node->user   = user;
    new_node->left   = &tree->nil;
    new_node->right  = &tree->nil;
    new_node->parent = &tree->nil;
    new_node->color  = RBT_RED;        /* new node is always RED */

    /*
     * Standard BST insertion: find the insertion point.
     */
    current = tree->root;
    parent  = &tree->nil;

    while (current != &tree->nil) {
        int cmp;
        parent = current;
        cmp = strcmp(user->data.name, current->user->data.name);
        if (cmp < 0) {
            current = current->left;
        } else if (cmp > 0) {
            current = current->right;
        } else {
            /* duplicate name — should not happen */
            free(new_node);
            log_error("rbt: duplicate username, insert skipped");
            return -1;
        }
    }

    new_node->parent = parent;

    /*
     * Branch: where does the new node go?
     *   - Root: tree was empty
     *   - Left child: name < parent
     *   - Right child: name > parent
     */
    if (parent == &tree->nil) {
        /* [ROOT] first node in the tree */
        tree->root = new_node;
    } else if (strcmp(user->data.name, parent->user->data.name) < 0) {
        parent->left = new_node;
    } else {
        parent->right = new_node;
    }

    tree->size++;

    /*
     * RBT INSERT FIXUP — restore red-black properties.
     * Only needed when the new node is NOT the root.
     */
    rbt_insert_fixup(tree, new_node);

    log_info("rbt: node inserted");
    return 0;
}

/*
 * rbt_insert_fixup — fixes red-black tree violations after insertion.
 *
 * The new node z is RED.  The only possible violation is:
 *   "a RED node has a RED parent"  (property: no two consecutive REDs)
 *
 * Loop invariant:
 *   - z is RED
 *   - If z.parent is root, z.parent is BLACK (handled at end)
 *   - The only violation (if any) is z and z.parent both RED
 *
 * Flowchart for each iteration (z.parent is RED):
 *
 *   ┌─ Is parent the LEFT or RIGHT child of grandparent?
 *   │
 *   ├─ LEFT ─────────────────────────────────────────────┐
 *   │                                                     │
 *   │  uncle = grandparent.right                          │
 *   │                                                     │
 *   │  ┌─ Case 1: uncle is RED ───────────────────┐      │
 *   │  │  - parent → BLACK                         │      │
 *   │  │  - uncle → BLACK                          │      │
 *   │  │  - grandparent → RED                      │      │
 *   │  │  - z = grandparent (move up)              │      │
 *   │  └───────────────────────────────────────────┘      │
 *   │                                                     │
 *   │  ┌─ Case 2/3: uncle is BLACK ───────────────┐      │
 *   │  │                                            │      │
 *   │  │  Case 2: LR型 (z is right child)           │      │
 *   │  │    - z = parent                            │      │
 *   │  │    - LEFT-ROTATE(z)                        │      │
 *   │  │    - Fall through to Case 3                │      │
 *   │  │                                            │      │
 *   │  │  Case 3: LL型 (z is left child)            │      │
 *   │  │    - parent → BLACK                        │      │
 *   │  │    - grandparent → RED                     │      │
 *   │  │    - RIGHT-ROTATE(grandparent)             │      │
 *   │  └────────────────────────────────────────────┘      │
 *   │                                                     │
 *   └─ RIGHT (symmetric) ──────────────────────────────────┘
 *      (mirror: swap left↔right, LL↔RR, LR↔RL)
 */
void rbt_insert_fixup(BST *tree, BSTnode *z) {
    BSTnode *uncle;

    /*
     * Loop while z's parent is RED (violation).
     * If parent is BLACK, there's nothing to fix.
     */
    while (z->parent->color == RBT_RED) {
        /*
         * Branch A: parent is LEFT child of grandparent
         */
        if (z->parent == z->parent->parent->left) {
            uncle = z->parent->parent->right;    /* uncle = sibling of parent */

            /*
             * Case A-1: uncle is RED
             *   Push blackness down from grandparent:
             *   recolor parent→BLACK, uncle→BLACK, grandparent→RED.
             *   Then move z up to grandparent (may need further fixup).
             */
            if (uncle->color == RBT_RED) {
                z->parent->color         = RBT_BLACK;
                uncle->color             = RBT_BLACK;
                z->parent->parent->color = RBT_RED;
                z = z->parent->parent;             /* move violation up */
                continue;
            }

            /*
             * uncle is BLACK (or NIL).  Determine rotation type.
             *
             * Case A-2: LR型 — z is the RIGHT (inner) child of parent.
             *   LEFT-ROTATE(parent) transforms LR → LL,
             *   then fall through to Case A-3.
             */
            if (z == z->parent->right) {
                z = z->parent;                     /* z moves up to parent */
                rbt_left_rotate(tree, z);          /* LR → LL */
            }

            /*
             * Case A-3: LL型 — z is the LEFT (outer) child of parent.
             *   Recolor parent→BLACK, grandparent→RED,
             *   RIGHT-ROTATE on grandparent to fix the structure.
             */
            z->parent->color         = RBT_BLACK;
            z->parent->parent->color = RBT_RED;
            rbt_right_rotate(tree, z->parent->parent);

        } else {
            /*
             * Branch B: parent is RIGHT child of grandparent (symmetric)
             */
            uncle = z->parent->parent->left;     /* uncle = sibling of parent */

            /*
             * Case B-1: uncle is RED
             *   Same as A-1: recolor and move up.
             */
            if (uncle->color == RBT_RED) {
                z->parent->color         = RBT_BLACK;
                uncle->color             = RBT_BLACK;
                z->parent->parent->color = RBT_RED;
                z = z->parent->parent;
                continue;
            }

            /*
             * Case B-2: RL型 — z is the LEFT (inner) child of parent.
             *   RIGHT-ROTATE(parent) transforms RL → RR,
             *   then fall through to Case B-3.
             */
            if (z == z->parent->left) {
                z = z->parent;                     /* z moves up to parent */
                rbt_right_rotate(tree, z);         /* RL → RR */
            }

            /*
             * Case B-3: RR型 — z is the RIGHT (outer) child of parent.
             *   Recolor parent→BLACK, grandparent→RED,
             *   LEFT-ROTATE on grandparent.
             */
            z->parent->color         = RBT_BLACK;
            z->parent->parent->color = RBT_RED;
            rbt_left_rotate(tree, z->parent->parent);
        }
    }

    /*
     * Root must always be BLACK (property 2).
     * This also handles the case where z is now the root.
     */
    tree->root->color = RBT_BLACK;
}

/* ================================================================
 * bst_find / bst_find_with_steps (unchanged search logic,
 * only nil sentinel replaces NULL)
 * ================================================================ */

ListPtr bst_find(BST *tree, const char *name) {
    BSTnode *current;

    if (tree == NULL || name == NULL) {
        log_error("rbt: find called with NULL tree or name");
        return NULL;
    }

    current = tree->root;
    while (current != &tree->nil) {
        int cmp = strcmp(name, current->user->data.name);
        if (cmp == 0) {
            log_info("rbt: user found");
            return current->user;
        } else if (cmp < 0) {
            current = current->left;
        } else {
            current = current->right;
        }
    }

    log_info("rbt: user not found");
    return NULL;
}

ListPtr bst_find_with_steps(BST *tree, const char *name, int *steps, int verbose) {
    BSTnode *current;
    int count = 0;

    if (tree == NULL || name == NULL) {
        log_error("rbt: find_with_steps called with NULL tree or name");
        if (steps != NULL) {
            *steps = 0;
        }
        return NULL;
    }

    if (verbose) {
        printf("  [RBT search path]\n");
    }

    current = tree->root;
    while (current != &tree->nil) {
        int cmp;
        count++;
        cmp = strcmp(name, current->user->data.name);

        if (verbose) {
            const char *dir;
            if (count == 1) {
                dir = "root";
            } else if (cmp < 0) {
                dir = "L";
            } else {
                dir = "R";
            }

            if (cmp == 0) {
                printf("    step %d: visit \"%s\" (%s) -> MATCH\n",
                       count, current->user->data.name, dir);
            } else if (cmp < 0) {
                printf("    step %d: visit \"%s\" (%s) -> \"%s\" < \"%s\", go LEFT\n",
                       count, current->user->data.name, dir,
                       name, current->user->data.name);
            } else {
                printf("    step %d: visit \"%s\" (%s) -> \"%s\" > \"%s\", go RIGHT\n",
                       count, current->user->data.name, dir,
                       name, current->user->data.name);
            }
        }

        if (cmp == 0) {
            if (steps != NULL) {
                *steps = count;
            }
            log_info("rbt: user found with steps");
            return current->user;
        } else if (cmp < 0) {
            current = current->left;
        } else {
            current = current->right;
        }
    }

    if (verbose) {
        printf("    (end of path, not found after %d steps)\n", count);
    }

    if (steps != NULL) {
        *steps = count;
    }
    log_info("rbt: user not found with steps");
    return NULL;
}

/* ================================================================
 * Auxiliary helpers for delete
 * ================================================================ */

/*
 * rbt_transplant: replace subtree rooted at u with subtree rooted at v.
 */
void rbt_transplant(BST *tree, BSTnode *u, BSTnode *v) {
    if (u->parent == &tree->nil) {
        tree->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    v->parent = u->parent;
}

/*
 * rbt_minimum: find the node with the smallest key in the subtree.
 */
BSTnode *rbt_minimum(BST *tree, BSTnode *node) {
    while (node->left != &tree->nil) {
        node = node->left;
    }
    return node;
}

/*
 * rbt_find_predecessor: finds the inorder predecessor of the given node
 * (the largest node in its left subtree).
 * Returns the predecessor and its parent via output parameters.
 */
void rbt_find_predecessor(BST *tree, BSTnode *node,
                                  BSTnode **out_pre, BSTnode **out_parent) {
    BSTnode *predecessor;
    BSTnode *parent;

    parent = node;
    predecessor = node->left;

    while (predecessor->right != &tree->nil) {
        parent = predecessor;
        predecessor = predecessor->right;
    }

    *out_pre    = predecessor;
    *out_parent = parent;
}

/* ================================================================
 * bst_delete (Red-Black Tree delete with double-black fixup)
 *
 * Strategy:
 *   1. Find the node z to delete by name.
 *   2. Determine y  = the node actually removed (or moved).
 *      - If z has ≤1 child:  y = z
 *      - If z has 2 children: y = predecessor(z)  [前驱替代]
 *   3. x = the child that replaces y (may be nil).
 *   4. If y was BLACK, we have a "double-black" at x → fixup.
 *   5. Free z (or y, depending).
 *
 * "Double-Black" means: after removing a BLACK node, the path
 * through x has one fewer BLACK node than other paths.
 * We resolve this via rbt_delete_fixup.
 * ================================================================ */
int bst_delete(BST *tree, const char *name) {
    BSTnode *z;          /* node to delete (by key) */
    BSTnode *y;          /* node actually removed from the tree */
    BSTnode *x;          /* node that replaces y */
    BSTnode *current;
    int y_original_color;

    if (tree == NULL || name == NULL) {
        log_error("rbt: delete called with NULL tree or name");
        return -1;
    }

    /* Step 1: find the node z by name */
    current = tree->root;
    while (current != &tree->nil) {
        int cmp = strcmp(name, current->user->data.name);
        if (cmp == 0) {
            break;
        } else if (cmp < 0) {
            current = current->left;
        } else {
            current = current->right;
        }
    }

    if (current == &tree->nil) {
        log_error("rbt: user not found for deletion");
        return -1;
    }

    z = current;

    /*
     * Step 2: determine y (the node to be removed/spliced out)
     * and record its original color.
     */
    y = z;
    y_original_color = y->color;

    /*
     * Branch on z's children:
     *
     *   ┌─ z has NO left child ─────────────────────────┐
     *   │  x = z.right                                   │
     *   │  TRANSPLANT(z, z.right)                        │
     *   │  y = z (already set), y removed as-is          │
     *   │                                                │
     *   ├─ z has NO right child ────────────────────────┤
     *   │  x = z.left                                    │
     *   │  TRANSPLANT(z, z.left)                         │
     *   │  y = z (already set), y removed as-is          │
     *   │                                                │
     *   └─ z has TWO children ──────────────────────────┘
     *      Find PREDECESSOR (前驱): largest in left subtree.
     *      y = predecessor  (this is the node removed)
     *      y_original_color = y.color
     *      x = y.left  (predecessor has no right child)
     *
     *      ┌─ predecessor IS z.left (adjacent) ─────┐
     *      │  x.parent = y                           │
     *      │  TRANSPLANT(z, y)                       │
     *      │  y.right = z.right                      │
     *      │  y.right.parent = y                     │
     *      │                                         │
     *      ├─ predecessor is deeper ─────────────────┤
     *      │  TRANSPLANT(y, y.left)  (y=x lifts up)  │
     *      │  y.left = z.left                        │
     *      │  y.left.parent = y                      │
     *      │  TRANSPLANT(z, y)                       │
     *      │  y.right = z.right                      │
     *      │  y.right.parent = y                     │
     *      └─────────────────────────────────────────┘
     *      y.color = z.color  (inherit color of deleted node)
     */
    if (z->left == &tree->nil) {
        /*
         * Case: z has no left child (only right child or nil).
         */
        x = z->right;
        rbt_transplant(tree, z, x);
    } else if (z->right == &tree->nil) {
        /*
         * Case: z has no right child (only left child).
         */
        x = z->left;
        rbt_transplant(tree, z, x);
    } else {
        /*
         * Case: z has TWO children.
         * Use inorder PREDECESSOR (前驱) to replace z.
         */
        BSTnode *predecessor;
        BSTnode *predecessor_parent;

        rbt_find_predecessor(tree, z, &predecessor, &predecessor_parent);

        y = predecessor;
        y_original_color = y->color;
        x = y->left;   /* predecessor has no right child; x may be nil */

        if (predecessor_parent == z) {
            /*
             * Sub-case: predecessor IS z's immediate left child.
             *   z.left = predecessor
             *   predecessor.right = nil (predecessor is max in left subtree)
             */
            x->parent = y; // when x is nil, this is safe
        } else {
            /*
             * Sub-case: predecessor is deeper in the left subtree.
             * Lift x to predecessor's position first.
             */
            rbt_transplant(tree, y, y->left);
            y->left = z->left;
            y->left->parent = y;// 保护y的信息，这里做顶替z的准备工作
        }

        /* transplant y into z's position */
        rbt_transplant(tree, z, y);//y彻底顶替掉z的生态位，但保持y原有信息
        y->right = z->right;
        y->right->parent = y;
        y->color = z->color;   /* inherit z's color */
    }

    /*
     * Step 3: if the removed node y was BLACK, we have a
     * "double-black" violation at x.  Call delete fixup.
     *
     * Double-black means: removing a BLACK node y leaves the
     * subtree at x with one fewer black node on its paths
     * compared to other parts of the tree.
     */
    if (y_original_color == RBT_BLACK) {
        rbt_delete_fixup(tree, x);
    }

    /*
     * Step 4: free the deleted node z.
     * Only free the BSTnode, NOT the ListNode it points to.
     */
    free(z);
    tree->size--;

    log_info("rbt: node deleted");
    return 0;
}

/*
 * rbt_delete_fixup — resolves the "double-black" violation.
 *
 * When a BLACK node is removed, its child x inherits an "extra"
 * blackness, becoming "double-black".  This function resolves
 * the violation by redistributing blackness through rotations
 * and recoloring.
 *
 * Loop: while x is not root AND x is BLACK (double-black):
 *
 *   x = node with "extra" blackness
 *   s = sibling of x
 *
 *   ┌─ x is LEFT child ────────────────────────────────────┐
 *   │                                                        │
 *   │  Case 1: sibling s is RED                              │
 *   │    → swap colors of s and x.parent                    │
 *   │    → LEFT-ROTATE on x.parent                          │
 *   │    → s = x.parent.right (new sibling after rotation)  │
 *   │    → Fall through to Cases 2-4                        │
 *   │                                                        │
 *   │  Case 2: s is BLACK, both of s's children are BLACK   │
 *   │    → s becomes RED (remove one black from sibling)    │
 *   │    → x = x.parent (push double-black upward)          │
 *   │                                                        │
 *   │  Case 3: s is BLACK, s's FAR child is BLACK,          │
 *   │           s's NEAR child is RED                        │
 *   │    → s.near_child becomes BLACK                       │
 *   │    → s becomes RED                                    │
 *   │    → RIGHT-ROTATE on s                                │
 *   │    → s = x.parent.right (new sibling)                 │
 *   │    → Fall through to Case 4                           │
 *   │                                                        │
 *   │  Case 4: s is BLACK, s's FAR child is RED             │
 *   │    → s.color = x.parent.color                         │
 *   │    → x.parent becomes BLACK                           │
 *   │    → s.far_child becomes BLACK                        │
 *   │    → LEFT-ROTATE on x.parent                          │
 *   │    → x = root (terminate loop)                        │
 *   │                                                        │
 *   └─ x is RIGHT child (symmetric) ─────────────────────────┘
 *
 * After loop: x.color = BLACK (remove extra blackness).
 */
void rbt_delete_fixup(BST *tree, BSTnode *x) {
    BSTnode *s;   /* sibling of x */

    while (x != tree->root && x->color == RBT_BLACK) { // x is the double-black node

        /*
         * Branch A: x is the LEFT child
         */
        if (x == x->parent->left) {
            s = x->parent->right;      /* sibling */

            /*
             * Case A-1: sibling s is RED.
             *   Swap colors: s→BLACK, parent→RED.
             *   LEFT-ROTATE on parent → s becomes grandparent.
             *   Update s to new sibling (was s.left before rotation).
             */
            if (s->color == RBT_RED) {
                s->color         = RBT_BLACK;
                x->parent->color = RBT_RED;
                rbt_left_rotate(tree, x->parent);
                s = x->parent->right;
                /* fall through to Cases 2-4 (now s is BLACK) */
            }

            /*
             * Case A-2: s is BLACK, both children of s are BLACK.
             *   Make s RED → sibling subtree loses one black.
             *   Push double-black up to parent.
             */
            if (s->left->color  == RBT_BLACK &&
                s->right->color == RBT_BLACK) {
                s->color = RBT_RED;
                x = x->parent;            /* move double-black upward */
            } else {
                /*
                 * Case A-3: s is BLACK, s's FAR child (s.right) is BLACK,
                 *           s's NEAR child (s.left) is RED.
                 *   s.left→BLACK, s→RED.
                 *   RIGHT-ROTATE on s → s.left becomes new sibling.
                 *   Update s.  Now s.right is RED → Case 4.
                 */
                if (s->right->color == RBT_BLACK) {
                    s->left->color = RBT_BLACK;
                    s->color       = RBT_RED;
                    rbt_right_rotate(tree, s);
                    s = x->parent->right;
                }

                /*
                 * Case A-4: s is BLACK, s's FAR child (s.right) is RED.
                 *   s takes parent's color, parent→BLACK, s.right→BLACK.
                 *   LEFT-ROTATE on parent.
                 *   x = root → exit loop.
                 */
                s->color         = x->parent->color;
                x->parent->color = RBT_BLACK;
                s->right->color  = RBT_BLACK;
                rbt_left_rotate(tree, x->parent);
                x = tree->root;            /* terminate */
            }

        } else {
            /*
             * Branch B: x is the RIGHT child (symmetric)
             */
            s = x->parent->left;       /* sibling */

            /*
             * Case B-1: sibling s is RED.
             */
            if (s->color == RBT_RED) {
                s->color         = RBT_BLACK;
                x->parent->color = RBT_RED;
                rbt_right_rotate(tree, x->parent);
                s = x->parent->left;
            }

            /*
             * Case B-2: s is BLACK, both children of s are BLACK.
             */
            if (s->right->color == RBT_BLACK &&
                s->left->color  == RBT_BLACK) {
                s->color = RBT_RED;
                x = x->parent;
            } else {
                /*
                 * Case B-3: s is BLACK, s's FAR child (s.left) is BLACK,
                 *           s's NEAR child (s.right) is RED.
                 */
                if (s->left->color == RBT_BLACK) {
                    s->right->color = RBT_BLACK;
                    s->color        = RBT_RED;
                    rbt_left_rotate(tree, s);
                    s = x->parent->left;
                }

                /*
                 * Case B-4: s is BLACK, s's FAR child (s.left) is RED.
                 */
                s->color         = x->parent->color;
                x->parent->color = RBT_BLACK;
                s->left->color   = RBT_BLACK;
                rbt_right_rotate(tree, x->parent);
                x = tree->root;
            }
        }
    }

    /*
     * Ensure x is BLACK (resolves double-black).
     */
    x->color = RBT_BLACK;
}

/* ================================================================
 * bst_inorder (traversal — skip nil sentinel)
 * ================================================================ */

static void bst_inorder_node(BSTnode *node, BSTnode *nil) {
    if (node == nil) {
        return;
    }

    bst_inorder_node(node->left, nil);

    printf("%s,%s,%s,%s,%s,%s\n",
           node->user->data.name,
           node->user->data.password,
           node->user->data.birthdate,
           node->user->data.phone,
           node->user->data.mobile,
           node->user->data.email);

    bst_inorder_node(node->right, nil);
}

void bst_inorder(BST *tree) {
    if (tree == NULL) {
        log_error("rbt: inorder called with NULL tree");
        return;
    }

    log_info("rbt: inorder traversal");
    bst_inorder_node(tree->root, &tree->nil);
}

/* ================================================================
 * bst_free (post-order — free tree nodes, not the nil sentinel)
 * ================================================================ */

static void bst_free_nodes(BSTnode *node, BSTnode *nil) {
    if (node == nil) {
        return;
    }

    bst_free_nodes(node->left, nil);
    bst_free_nodes(node->right, nil);

    /*
     * Only free the BSTnode itself.
     * node->user points to a ListNode owned by the linked list
     * in user_store.c, which will be freed separately.
     */
    free(node);
}

void bst_free(BST *tree) {
    if (tree == NULL) {
        return;
    }

    bst_free_nodes(tree->root, &tree->nil);

    /* reset to empty state */
    tree->root = &tree->nil;
    tree->size = 0;
    log_info("rbt: tree freed");
}
