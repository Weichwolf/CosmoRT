/* CosmoRT Red-Black Tree — insert fixup, erase, traversal
 *
 * Standard RB-tree algorithms. Intrusive nodes, no allocation.
 * Based on CLRS + Linux kernel rbtree.c (simplified).
 */

#include "core/rbtree.h"

static void rotate_left(rb_root_t *root, rb_node_t *x) {
    rb_node_t *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent)
        root->node = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rotate_right(rb_root_t *root, rb_node_t *x) {
    rb_node_t *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (!x->parent)
        root->node = y;
    else if (x == x->parent->right)
        x->parent->right = y;
    else
        x->parent->left = y;
    y->right = x;
    x->parent = y;
}

void rb_insert_fixup(rb_root_t *root, rb_node_t *z) {
    while (z->parent && z->parent->color == RB_RED) {
        rb_node_t *gp = z->parent->parent;
        if (!gp) break;
        if (z->parent == gp->left) {
            rb_node_t *y = gp->right;
            if (y && y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                gp->color = RB_RED;
                z = gp;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(root, z);
                }
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_right(root, z->parent->parent);
            }
        } else {
            rb_node_t *y = gp->left;
            if (y && y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                gp->color = RB_RED;
                z = gp;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(root, z);
                }
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_left(root, z->parent->parent);
            }
        }
    }
    root->node->color = RB_BLACK;
}

/* Transplant: replace u with v in the tree */
static void transplant(rb_root_t *root, rb_node_t *u, rb_node_t *v) {
    if (!u->parent)
        root->node = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;
    if (v) v->parent = u->parent;
}

static rb_node_t *tree_minimum(rb_node_t *x) {
    while (x->left) x = x->left;
    return x;
}

void rb_erase(rb_root_t *root, rb_node_t *z) {
    /* Update leftmost cache */
    if (root->leftmost == z) {
        rb_node_t *next = rb_next(z);
        root->leftmost = next;
    }

    rb_node_t *y = z, *x;
    rb_node_t *x_parent = 0;
    int y_orig_color = y->color;

    if (!z->left) {
        x = z->right;
        x_parent = z->parent;
        transplant(root, z, z->right);
    } else if (!z->right) {
        x = z->left;
        x_parent = z->parent;
        transplant(root, z, z->left);
    } else {
        y = tree_minimum(z->right);
        y_orig_color = y->color;
        x = y->right;
        if (y->parent == z) {
            x_parent = y;
        } else {
            x_parent = y->parent;
            transplant(root, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(root, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    /* Fixup only if we removed a black node */
    if (y_orig_color == RB_BLACK && root->node) {
        while (x != root->node && (!x || x->color == RB_BLACK)) {
            if (x == (x_parent ? x_parent->left : 0)) {
                rb_node_t *w = x_parent->right;
                if (w && w->color == RB_RED) {
                    w->color = RB_BLACK;
                    x_parent->color = RB_RED;
                    rotate_left(root, x_parent);
                    w = x_parent->right;
                }
                if ((!w->left || w->left->color == RB_BLACK) &&
                    (!w->right || w->right->color == RB_BLACK)) {
                    w->color = RB_RED;
                    x = x_parent;
                    x_parent = x->parent;
                } else {
                    if (!w->right || w->right->color == RB_BLACK) {
                        if (w->left) w->left->color = RB_BLACK;
                        w->color = RB_RED;
                        rotate_right(root, w);
                        w = x_parent->right;
                    }
                    w->color = x_parent->color;
                    x_parent->color = RB_BLACK;
                    if (w->right) w->right->color = RB_BLACK;
                    rotate_left(root, x_parent);
                    x = root->node;
                }
            } else {
                rb_node_t *w = x_parent->left;
                if (w && w->color == RB_RED) {
                    w->color = RB_BLACK;
                    x_parent->color = RB_RED;
                    rotate_right(root, x_parent);
                    w = x_parent->left;
                }
                if ((!w->right || w->right->color == RB_BLACK) &&
                    (!w->left || w->left->color == RB_BLACK)) {
                    w->color = RB_RED;
                    x = x_parent;
                    x_parent = x->parent;
                } else {
                    if (!w->left || w->left->color == RB_BLACK) {
                        if (w->right) w->right->color = RB_BLACK;
                        w->color = RB_RED;
                        rotate_left(root, w);
                        w = x_parent->left;
                    }
                    w->color = x_parent->color;
                    x_parent->color = RB_BLACK;
                    if (w->left) w->left->color = RB_BLACK;
                    rotate_right(root, x_parent);
                    x = root->node;
                }
            }
        }
        if (x) x->color = RB_BLACK;
    }
}

rb_node_t *rb_next(rb_node_t *node) {
    if (!node) return 0;
    if (node->right) {
        node = node->right;
        while (node->left) node = node->left;
        return node;
    }
    while (node->parent && node == node->parent->right)
        node = node->parent;
    return node->parent;
}
