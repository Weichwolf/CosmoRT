/* CosmoRT Page Allocator — bitmap-based, O(1) free, no arena */
#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>
#include <stddef.h>

/* Initialize from UEFI memory map heap region */
void page_alloc_init(uint8_t *base, size_t size);

/* Allocate one zeroed 4KB page. Returns kernel-virtual (identity-mapped). */
void *page_alloc(void);

/* Free a single page */
void page_free(void *page);

/* Allocate N contiguous zeroed pages (for kernel stacks, etc.) */
void *pages_alloc(int n);

/* Free N contiguous pages */
void pages_free(void *base, int n);

/* Stats */
int page_alloc_total(void);
int page_alloc_free(void);

#endif
