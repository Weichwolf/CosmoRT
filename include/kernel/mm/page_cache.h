/* CosmoRT Page Cache — maps (inode, offset) → physical page */
#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <stdint.h>

uint64_t page_cache_lookup(uint64_t ino, uint64_t offset);

void page_cache_insert(uint64_t ino, uint64_t offset, uint64_t phys);

void page_cache_remove(uint64_t ino, uint64_t offset);

void page_cache_invalidate_ino(uint64_t ino);

void page_cache_evict(uint64_t phys);

#endif
