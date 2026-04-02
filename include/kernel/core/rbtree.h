/* CosmoRT Red-Black Tree — intrusive, Linux-style
 *
 * Embed rb_node in your struct. Tree sorted by caller's comparison.
 * O(log n) insert/delete/search. O(1) leftmost (cached).
 */
#ifndef RBTREE_H
#define RBTREE_H

#include <stdint.h>
#include <stddef.h>

typedef struct rb_node {
    struct rb_node *left, *right, *parent;
    int color; /* 0 = black, 1 = red */
} rb_node_t;

typedef struct rb_root {
    rb_node_t *node;     /* root */
    rb_node_t *leftmost; /* cached minimum — O(1) access */
} rb_root_t;

#define RB_ROOT_INIT { 0, 0 }

#define RB_RED   1
#define RB_BLACK 0

/* Insert node into tree. Caller must set node->left = node->right = NULL
 * and link node->parent + parent's child pointer before calling. */
void rb_insert_fixup(rb_root_t *root, rb_node_t *node);

/* Remove node from tree. */
void rb_erase(rb_root_t *root, rb_node_t *node);

/* Next node in in-order traversal (NULL if rightmost). */
rb_node_t *rb_next(rb_node_t *node);

/* Helper: link a new node as child of parent, then fixup.
 * link points to &parent->left or &parent->right. */
static inline void rb_link_node(rb_node_t *node, rb_node_t *parent,
                                rb_node_t **link) {
    node->parent = parent;
    node->left = node->right = 0;
    node->color = RB_RED;
    *link = node;
}

/* container_of for rb_node → enclosing struct */
#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#endif
