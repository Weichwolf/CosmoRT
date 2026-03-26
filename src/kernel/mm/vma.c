/* CosmoRT VMA — AVL tree for per-process virtual memory areas */

#include "mm/vma.h"
#include "mm/slab.h"
#include "hw/serial.h"

#define VMA_MAX 8192  /* 384KB — mprotect splits multiply VMA count */
static vma_t vma_pool[VMA_MAX];
static slab_t vma_slab;

void vma_init(void) {
    slab_init(&vma_slab, vma_pool, sizeof(vma_t), VMA_MAX);
    serial_puts("vma: init (slab)\n");
}

vma_t *vma_alloc_raw(void) {
    vma_t *v = (vma_t *)slab_alloc(&vma_slab);
    if (!v) serial_puts("vma: EXHAUSTED!\n");
    return v;
}

void vma_free(vma_t *v) {
    slab_free(&vma_slab, v);
}

/* ── AVL helpers ─────────────────────────────────── */

static int height(vma_t *n) {
    return n ? n->height : 0;
}

static int balance_factor(vma_t *n) {
    return n ? height(n->left) - height(n->right) : 0;
}

static void update_height(vma_t *n) {
    int l = height(n->left);
    int r = height(n->right);
    n->height = (l > r ? l : r) + 1;
}

static vma_t *rotate_right(vma_t *y) {
    vma_t *x = y->left;
    vma_t *t = x->right;
    x->right = y;
    y->left = t;
    update_height(y);
    update_height(x);
    return x;
}

static vma_t *rotate_left(vma_t *x) {
    vma_t *y = x->right;
    vma_t *t = y->left;
    y->left = x;
    x->right = t;
    update_height(x);
    update_height(y);
    return y;
}

static vma_t *rebalance(vma_t *n) {
    update_height(n);
    int bf = balance_factor(n);
    if (bf > 1) {
        if (balance_factor(n->left) < 0)
            n->left = rotate_left(n->left);
        return rotate_right(n);
    }
    if (bf < -1) {
        if (balance_factor(n->right) > 0)
            n->right = rotate_right(n->right);
        return rotate_left(n);
    }
    return n;
}

/* ── Find ────────────────────────────────────────── */

vma_t *vma_find(vma_t *root, uint64_t addr) {
    vma_t *n = root;
    while (n) {
        if (addr < n->start)
            n = n->left;
        else if (addr >= n->end)
            n = n->right;
        else
            return n; /* addr in [start, end) */
    }
    return 0;
}

/* ── Find overlap ────────────────────────────────── */

/* Find any VMA overlapping [start, end). AVL tree is keyed by vma->start.
 * A VMA overlaps if vma->start < end && vma->end > start. */
vma_t *vma_find_overlap(vma_t *root, uint64_t start, uint64_t end) {
    if (!root) return 0;
    /* If this node overlaps, return it */
    if (root->start < end && root->end > start)
        return root;
    /* If left subtree might contain an overlap (left nodes have start < root->start,
     * but their end could extend into [start, end)) — check left if start < root->start */
    if (root->left && start < root->start) {
        vma_t *r = vma_find_overlap(root->left, start, end);
        if (r) return r;
    }
    /* Check right subtree if range extends past this node */
    if (root->right && end > root->start) {
        vma_t *r = vma_find_overlap(root->right, start, end);
        if (r) return r;
    }
    return 0;
}

/* ── Insert ──────────────────────────────────────── */

static vma_t *insert_node(vma_t *root, vma_t *node) {
    if (!root) return node;
    if (node->start < root->start)
        root->left = insert_node(root->left, node);
    else
        root->right = insert_node(root->right, node);
    return rebalance(root);
}

vma_t *vma_insert(vma_t **root, uint64_t start, uint64_t end, int prot, int flags) {
    vma_t *v = vma_alloc_raw();
    if (!v) return 0;
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->file_ino = 0;
    v->file_offset = 0;
    v->file_backend = 0;
    v->left = 0;
    v->right = 0;
    v->height = 1;
    *root = insert_node(*root, v);
    return v;
}

/* ── Remove ──────────────────────────────────────── */

static vma_t *min_node(vma_t *n) {
    while (n->left) n = n->left;
    return n;
}

static vma_t *remove_node(vma_t *root, vma_t *target, vma_t **removed) {
    if (!root) return 0;
    if (target->start < root->start) {
        root->left = remove_node(root->left, target, removed);
    } else if (target->start > root->start) {
        root->right = remove_node(root->right, target, removed);
    } else if (root == target) {
        *removed = root;
        if (!root->left) return root->right;
        if (!root->right) return root->left;
        /* Two children: replace with in-order successor.
         * Detach successor from tree BEFORE marking it as removed,
         * so rebalancing during detach never touches the freed node. */
        vma_t *succ = min_node(root->right);
        /* Copy data from successor into root */
        root->start = succ->start;
        root->end = succ->end;
        root->prot = succ->prot;
        root->flags = succ->flags;
        /* Detach successor from subtree (rebalancing happens here) */
        root->right = remove_node(root->right, succ, &(vma_t *){0});
        /* Now mark successor as the node to free */
        *removed = succ;
    } else {
        /* Same start but different node — search both subtrees */
        root->right = remove_node(root->right, target, removed);
    }
    return rebalance(root);
}

void vma_remove(vma_t **root, vma_t *node) {
    vma_t *removed = 0;
    *root = remove_node(*root, node, &removed);
    if (removed)
        vma_free(removed);
}

/* ── Find free gap (grows down from base) ────────── */

/* Reverse in-order traversal (right, node, left) visits VMAs from highest
 * to lowest address. Track gap_end starting at base. O(1) extra stack
 * beyond the traversal stack (pointer-sized entries, not 16KB arrays). */

uint64_t vma_find_free(vma_t *root, uint64_t base, uint64_t size) {
    if (!root) {
        /* No VMAs at all — place at base - size */
        return (base >= size) ? base - size : 0;
    }

    /* Reverse in-order traversal: visit VMAs from highest to lowest.
     * For each VMA, check if there's a gap between its end and gap_end. */
    vma_t *stack[64];
    int sp = 0;
    vma_t *cur = root;
    uint64_t gap_end = base;
    uint64_t result = 0;

    /* Reverse in-order: right first, then node, then left */
    while (cur || sp > 0) {
        while (cur) {
            if (sp < 64) stack[sp++] = cur;
            cur = cur->right;
        }
        if (sp > 0) {
            cur = stack[--sp];
            /* Check gap between cur->end and gap_end */
            if (gap_end < size) { cur = cur->left; continue; }
            if (cur->end <= gap_end && gap_end - cur->end >= size) {
                uint64_t addr = (gap_end - size) & ~0xFFFULL;
                if (addr >= cur->end) {
                    result = addr;
                    return result;
                }
            }
            /* Advance gap_end downward — never above base */
            if (cur->start < gap_end)
                gap_end = cur->start;
            cur = cur->left;
        }
    }

    /* Gap before the first (lowest) VMA */
    if (gap_end >= size) {
        uint64_t addr = (gap_end - size) & ~0xFFFULL;
        if (addr >= 0x1000) /* don't map NULL page */
            return addr;
    }

    return 0;
}

/* Find free gap at or above 'start', growing upward.
 * In-order traversal (lowest→highest), track gap_start. */
uint64_t vma_find_free_above(vma_t *root, uint64_t start, uint64_t size) {
    uint64_t limit = 0x800000000000ULL; /* user space ceiling */
    if (!root) {
        uint64_t addr = (start + 0xFFF) & ~0xFFFULL;
        return (addr + size <= limit) ? addr : 0;
    }

    vma_t *stack[64];
    int sp = 0;
    vma_t *cur = root;
    uint64_t gap_start = (start + 0xFFF) & ~0xFFFULL;

    /* In-order: left first, then node, then right */
    while (cur || sp > 0) {
        while (cur) {
            if (sp < 64) stack[sp++] = cur;
            cur = cur->left;
        }
        if (sp > 0) {
            cur = stack[--sp];
            /* Gap between gap_start and cur->start */
            if (cur->start > gap_start && cur->start - gap_start >= size)
                return gap_start;
            /* Advance gap_start past this VMA */
            if (cur->end > gap_start)
                gap_start = (cur->end + 0xFFF) & ~0xFFFULL;
            cur = cur->right;
        }
    }

    /* Gap after the last (highest) VMA */
    if (gap_start + size <= limit)
        return gap_start;

    return 0;
}
