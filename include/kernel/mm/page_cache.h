/* CosmoRT Page Cache — maps (inode, offset) → physical page
 *
 * When two processes mmap the same file region MAP_SHARED,
 * both get the same physical page via this cache. */
#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <stdint.h>

/* Look up cached page. Returns physical address, or 0 if not found. */
uint64_t page_cache_lookup(uint64_t ino, uint64_t offset);

/* Insert page into cache. offset must be page-aligned. */
void page_cache_insert(uint64_t ino, uint64_t offset, uint64_t phys);

/* Remove page from cache by (ino, offset) key. */
void page_cache_remove(uint64_t ino, uint64_t offset);

/* Invalidate all cached pages for an inode (called on write/truncate). */
void page_cache_invalidate_ino(uint64_t ino);

#endif
