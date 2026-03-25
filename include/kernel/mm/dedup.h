/* CosmoRT RAM Dedup — KSM-style page deduplication on RT-Core
 *
 * MM↔Dedup boundary: MM knows pages+PTEs, Dedup knows hashes.
 * Neither crosses the other's abstraction. */
#ifndef DEDUP_H
#define DEDUP_H

#include <stdint.h>

/* ── MM exports to Dedup ─────────────────────────── */

/* Next anonymous user page that is a dedup candidate (refcount==1, cold).
 * Returns 0 + *phys_out set, or -1 if scan cycle complete. */
int  mm_dedup_scan_next(uint64_t *phys_out);

/* Reset scan cursor to beginning. */
void mm_dedup_scan_reset(void);

/* Rewrite all PTEs pointing at victim → keep (read-only + COW).
 * Decrements victim refcount. Returns number of rewritten PTEs. */
int  mm_dedup_merge(uint64_t keep_phys, uint64_t victim_phys);

/* ── Dedup exports to MM (lifecycle callbacks) ───── */

/* Page freed → remove hash from table. */
void dedup_on_page_free(uint64_t phys);

/* COW break: old page copied → invalidate old hash. */
void dedup_on_cow_break(uint64_t old_phys, uint64_t new_phys);

/* ── Lifecycle ───────────────────────────────────── */

void dedup_init(void);

/* Stats (for /proc/ksm) */
typedef struct {
    uint64_t pages_scanned;
    uint64_t pages_shared;   /* currently shared via dedup */
    uint64_t pages_merged;   /* total merges performed */
    uint64_t bytes_saved;
} dedup_stats_t;

void dedup_get_stats(dedup_stats_t *out);

#endif
