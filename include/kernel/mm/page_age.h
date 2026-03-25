/* CosmoRT Page Age Tracking — Accessed/Dirty hardware bit sampling
 *
 * 2-bit age counter per physical page frame.
 * Periodically read + clear Accessed bit → shift counter.
 * Only PAGE_COLD pages are dedup candidates. */
#ifndef PAGE_AGE_H
#define PAGE_AGE_H

#include <stdint.h>

enum page_temp { PAGE_HOT = 0, PAGE_WARM = 1, PAGE_COLD = 2 };

/* Walk all user PTEs, sample Accessed bit, age pages. Bounded work. */
void mm_age_pages(void);

/* Query temperature of a physical page. */
enum page_temp mm_page_temperature(uint64_t phys);

/* Initialize age tracking (call after page_alloc_init). */
void page_age_init(uint64_t max_pfn);

#endif
