/* CosmoRT GUP — get_user_pages-equivalent helper.
 *
 * Resolve a user virtual address to its physical page, demand-faulting
 * the page if not yet present. Linux uses get_user_pages(GUP_FAST) for
 * the same job inside futex_get_key. We need it for FUTEX_SHARED so
 * cross-process WAKE on a page only one side has touched still resolves
 * to the same PA-key. Also reusable for any kernel path that needs to
 * pin a user page without going through copy_*_user. */
#ifndef MM_GUP_H
#define MM_GUP_H

#include <stdint.h>

struct process;

/* Translate user-VA to its backing PA, page-aligning va internally.
 * Demand-faults the page when not yet mapped (file-backed via page_cache
 * or vfs_pread_by_ino, anonymous via alloc_page).
 *
 * Caller must NOT hold p->lock. Runs in process/syscall context.
 * write=0 keeps the page read-only (no CoW break). write=1 is reserved
 * for future use (currently rejects CoW pages with PA=0).
 *
 * Returns the page-aligned physical address on success, 0 on failure
 * (no VMA, PROT_NONE, alloc-fail, file backend error). */
uint64_t mm_gup_one(struct process *p, uint64_t va, int write);

#endif
