/* CosmoRT Page Cache — maps (inode, offset) → physical page
 *
 * When two processes mmap the same file region MAP_SHARED,
 * both get the same physical page via this cache.
 *
 * For MAP_PRIVATE the cache holds the clean copy; consumers map it
 * read-only with PTE_COW so the existing COW path forks a private
 * page on first write. Linux page-cache semantics. */
#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <stdint.h>

/* Look up cached page. Returns physical address, or 0 if not found.
 * Does NOT incref — caller must ensure the page survives until used.
 * Prefer page_cache_get_or_load for the demand-paging path. */
uint64_t page_cache_lookup(uint64_t ino, uint64_t offset);

/* Atomic get-or-load: returns physical address with refcount incremented
 * (caller must page_free on undo). On miss, reads `len` bytes via
 * vfs_pread_by_ino(backend, ino, ...) into a fresh page, inserts it into
 * the cache (which holds its own ref), and returns it.
 *
 * Race-safe: if another thread loads the same (ino, offset) concurrently,
 * one wins the insert, the loser frees its tentative page and returns
 * the winner's. Returns 0 only on allocation/I-O failure. */
uint64_t page_cache_get_or_load(int backend, uint64_t ino, uint64_t offset);

/* Insert page into cache. offset must be page-aligned.
 * Cache takes its own ref (page_incref). Caller still owns the original ref. */
void page_cache_insert(uint64_t ino, uint64_t offset, uint64_t phys);

/* Remove page from cache by (ino, offset) key. */
void page_cache_remove(uint64_t ino, uint64_t offset);

/* Invalidate all cached pages for an inode (called on write/truncate). */
void page_cache_invalidate_ino(uint64_t ino);

#endif
