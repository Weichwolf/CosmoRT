/* CosmoRT VMA — AVL tree of virtual memory areas per process */
#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stddef.h>

#define __KERNEL__
#include "linux/abi.h"

#define VMA_LOCKED    0x100
#define VMA_SHARED    0x200
#define VMA_HUGEPAGE  0x400

typedef struct vma {
    uint64_t start;
    uint64_t end;
    int prot;
    int flags;
    uint64_t file_ino;
    uint64_t file_offset;
    int file_backend;
    struct vma *left;
    struct vma *right;
    int height;
} vma_t;

void vma_init(void);

vma_t *vma_find(vma_t *root, uint64_t addr);

vma_t *vma_insert(vma_t **root, uint64_t start, uint64_t end, int prot, int flags);

void vma_remove(vma_t **root, vma_t *node);

uint64_t vma_find_free(vma_t *root, uint64_t base, uint64_t size);

vma_t *vma_find_overlap(vma_t *root, uint64_t start, uint64_t end);

void vma_free(vma_t *v);

void vma_free_tree(vma_t *node);

vma_t *vma_alloc_raw(void);

#endif
