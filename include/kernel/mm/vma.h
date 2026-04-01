/* CosmoRT VMA — AVL tree of virtual memory areas per process */
#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stddef.h>

/* PROT_* and MAP_* flags come from linux.h (via syscall.h or direct) */
#define __KERNEL__
#include "linux/abi.h"

/* VMA flags (internal, stored in vma_t.flags upper bits) */
#define VMA_LOCKED    0x100  /* pages pre-faulted, no demand paging */
#define VMA_SHARED    0x200  /* MAP_SHARED: fork shares physical pages */
#define VMA_HUGEPAGE   0x400  /* eligible for transparent 2MB huge pages */
#define VMA_NOHUGEPAGE 0x1000 /* explicitly disable THP for this VMA */
#define VMA_GROWSDOWN  0x800  /* stack: auto-expand downward on page fault */

typedef struct vma {
    uint64_t start;      /* page-aligned start address */
    uint64_t end;        /* page-aligned end address (exclusive) */
    int prot;            /* PROT_READ | PROT_WRITE | PROT_EXEC */
    int flags;           /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED */
    uint64_t file_ino;   /* backing file inode (0 = anonymous) */
    uint64_t file_offset; /* offset in file where this VMA starts */
    int file_backend;    /* VFS_BACKEND_RAM or VFS_BACKEND_EXT2 */
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

/* Find the lowest VMA whose start > addr. For VM_GROWSDOWN stack expansion. */
vma_t *vma_find_above(vma_t *root, uint64_t addr);

/* Free a VMA back to slab (without removing from tree — for internal use). */
void vma_free(vma_t *v);

/* Free all VMAs in a tree recursively. */
void vma_free_tree(vma_t *node);

/* Allocate a raw VMA from slab (for splitting). */
vma_t *vma_alloc_raw(void);

#endif
