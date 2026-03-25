/* CosmoRT Page Allocator — buddy system, O(1) single-page alloc */
#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>
#include <stddef.h>

/* Compat stub (real init via page_alloc_add_uefi_regions) */
void page_alloc_init(uint8_t *base, size_t size);

/* Register all UEFI EfiConventionalMemory regions with the buddy allocator.
 * mmap_virt: virtual address of UEFI memory map
 * mmap_size: total size in bytes
 * desc_size: size of one EFI_MEMORY_DESCRIPTOR */
void page_alloc_add_uefi_regions(void *mmap_virt, uint64_t mmap_size,
                                  uint64_t desc_size);

/* Allocate one zeroed 4KB page. Returns kernel-virtual (direct-mapped). */
void *page_alloc(void);

/* Free a single page */
void page_free(void *page);

/* Allocate N contiguous zeroed pages (rounded up to power of 2) */
void *pages_alloc(int n);

/* Free N contiguous pages (must match the n passed to pages_alloc) */
void pages_free(void *base, int n);

/* Stats */
int page_alloc_total(void);
int page_alloc_free(void);
uint64_t page_alloc_max_pfn(void);

/* Page refcount (COW support).
 * phys = physical address of 4KB page (page-aligned). */
void page_incref(uint64_t phys);     /* refcount++ (atomic) */
int  page_decref(uint64_t phys);     /* refcount--, returns new count */
int  page_refcount(uint64_t phys);   /* current refcount */

#endif
