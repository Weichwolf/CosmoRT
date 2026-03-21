/* CosmoRT VMA — AVL tree of virtual memory areas per process */
#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stddef.h>

/* Protection flags (Linux-compatible) */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* Map flags (Linux-compatible) — also in syscall.h */
#ifndef MAP_FIXED
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#endif

/* VMA flags (internal, stored in vma_t.flags upper bits) */
#define VMA_LOCKED    0x100  /* pages pre-faulted, no demand paging */

typedef struct vma {
    uint64_t start;      /* page-aligned start address */
    uint64_t end;        /* page-aligned end address (exclusive) */
    int prot;            /* PROT_READ | PROT_WRITE | PROT_EXEC */
    int flags;           /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED */
    /* AVL tree linkage */
    struct vma *left;
    struct vma *right;
    int height;
} vma_t;

/* Initialize the VMA slab pool. Call once at boot. */
void vma_init(void);

/* Find VMA containing addr. Returns NULL if none. */
vma_t *vma_find(vma_t *root, uint64_t addr);

/* Insert a new VMA [start, end). Returns the new node, or NULL on failure. */
vma_t *vma_insert(vma_t **root, uint64_t start, uint64_t end, int prot, int flags);

/* Remove a VMA from the tree and free it back to slab. */
void vma_remove(vma_t **root, vma_t *node);

/* Find a free gap of at least `size` bytes, searching downward from `base`.
 * Returns a VMA stub with .start set to the gap address, or NULL if no gap.
 * (Actually returns the gap start address, 0 on failure.) */
uint64_t vma_find_free(vma_t *root, uint64_t base, uint64_t size);

/* Find first VMA overlapping [start, end). O(log n) AVL search. */
vma_t *vma_find_overlap(vma_t *root, uint64_t start, uint64_t end);

/* Free a VMA back to slab (without removing from tree — for internal use). */
void vma_free(vma_t *v);

/* Allocate a raw VMA from slab (for splitting). */
vma_t *vma_alloc_raw(void);

#endif
