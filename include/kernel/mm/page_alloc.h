/* CosmoRT Page Allocator — buddy system, O(1) single-page alloc */
#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>
#include <stddef.h>

void page_alloc_init(uint8_t *base, size_t size);

void page_alloc_add_uefi_regions(void *mmap_virt, uint64_t mmap_size,
                                  uint64_t desc_size);

void *page_alloc(void);

void page_free(void *page);

void *pages_alloc(int n);

void pages_free(void *base, int n);

void *huge_page_alloc(void);

void huge_page_free(void *page);

int page_alloc_total(void);
int page_alloc_free(void);
uint64_t page_alloc_max_pfn(void);

void page_incref(uint64_t phys);
int  page_decref(uint64_t phys);
int  page_refcount(uint64_t phys);

#endif
