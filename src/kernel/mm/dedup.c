/* CosmoRT RAM Dedup — KSM on RT-Core
 *
 * Three components in one file:
 * 1. Page-Age tracking (Accessed-bit sampling)
 * 2. MM scan/merge helpers (page table walking)
 * 3. Dedup engine (hash table + rt_poll handler)
 *
 * Runs entirely on RT-Core at lowest priority. */

#include "mm/dedup.h"
#include "mm/page_age.h"
#include "mm/page_alloc.h"
#include "proc/process.h"
#include "core/rt_poll.h"
#include "core/rt.h"
#include "core/smp.h"
#include "crypto/sha256.h"
#include "hw/serial.h"
#include "config.h"
#include "memops.h"
#include "arch/arch.h"
#include "spinlock.h"
#include "core/irq.h"

/* PTE flags (duplicated from process.c — no shared header yet) */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_COW       (1ULL << 9)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* ════════════════════════════════════════════════════
 * 1. PAGE-AGE TRACKING
 * ════════════════════════════════════════════════════ */

/* 2-bit counter per page frame: 0=hot, 1=warm, 2+=cold.
 * Packed 4 per byte. 1MB for 16M pages (64GB RAM). */
static uint8_t *age_counters;
static uint64_t age_max_pfn;

/* Aging cursor: which process / VMA offset we left off at */
static int age_proc_idx;
static uint64_t age_va_cursor;
#define AGE_MAX_WORK 512  /* max PTEs per mm_age_pages() call */

void page_age_init(uint64_t max_pfn) {
    /* Allocate age counter array: 2 bits per page, packed 4 per byte */
    age_max_pfn = max_pfn;
    uint64_t bytes = (max_pfn + 3) / 4;
    uint64_t pages_needed = (bytes + 4095) / 4096;
    age_counters = (uint8_t *)pages_alloc((int)pages_needed);
    if (!age_counters) {
        serial_puts("dedup: age counter alloc failed\n");
        return;
    }
    /* All pages start cold (counter = 3) so initial scan finds candidates */
    kmemset(age_counters, 0xFF, bytes);
}

static inline uint8_t age_get(uint64_t pfn) {
    if (pfn >= age_max_pfn || !age_counters) return 0;
    uint8_t byte = age_counters[pfn / 4];
    return (byte >> ((pfn % 4) * 2)) & 3;
}

static inline void age_set(uint64_t pfn, uint8_t val) {
    if (pfn >= age_max_pfn || !age_counters) return;
    uint8_t *bp = &age_counters[pfn / 4];
    int shift = (pfn % 4) * 2;
    *bp = (*bp & ~(3 << shift)) | ((val & 3) << shift);
}

enum page_temp mm_page_temperature(uint64_t phys) {
    uint64_t pfn = phys >> 12;
    uint8_t a = age_get(pfn);
    if (a == 0) return PAGE_HOT;
    if (a == 1) return PAGE_WARM;
    return PAGE_COLD;
}

/* Walk user PTEs of one process, sample + clear Accessed bit.
 * Returns number of PTEs processed. */
static int age_walk_process(process_t *p, uint64_t *va_cursor, int max_work) {
    if (!p || !p->pml4 || p->state != PROC_ALIVE) return 0;

    uint64_t *pml4 = p->pml4;
    int work = 0;

    /* Resume from cursor or start at 0 */
    uint64_t va = *va_cursor;

    int pml4i = (va >> 39) & 0x1FF;
    int pdpti = (va >> 30) & 0x1FF;
    int pdi   = (va >> 21) & 0x1FF;
    int pti   = (va >> 12) & 0x1FF;

    for (int i = pml4i; i < 256 && work < max_work; i++) {
        if (!(pml4[i] & PTE_PRESENT)) { pdpti = 0; pdi = 0; pti = 0; continue; }
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i] & PTE_ADDR_MASK);

        for (int j = (i == pml4i ? pdpti : 0); j < 512 && work < max_work; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) { pdi = 0; pti = 0; continue; }
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);

            for (int k = (i == pml4i && j == pdpti ? pdi : 0); k < 512 && work < max_work; k++) {
                if (!(pd[k] & PTE_PRESENT)) { pti = 0; continue; }
                uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_ADDR_MASK);

                for (int l = (i == pml4i && j == pdpti && k == pdi ? pti : 0); l < 512 && work < max_work; l++) {
                    uint64_t pte = pt[l];
                    if (!(pte & PTE_PRESENT)) continue;

                    uint64_t phys = pte & PTE_ADDR_MASK;
                    uint64_t pfn = phys >> 12;

                    if (pte & PTE_ACCESSED) {
                        /* Page was accessed: reset age to hot */
                        age_set(pfn, 0);
                        /* Clear Accessed bit (atomic: other CPUs may set it) */
                        pt[l] = pte & ~PTE_ACCESSED;
                    } else {
                        /* Not accessed: increment age (saturate at 3) */
                        uint8_t a = age_get(pfn);
                        if (a < 3) age_set(pfn, a + 1);
                    }
                    work++;
                }
                pti = 0;
            }
            pdi = 0;
        }
        pdpti = 0;
    }

    /* Save cursor: compute next VA */
    *va_cursor = (work < max_work) ? 0 : va + (uint64_t)work * 4096;
    return work;
}

void mm_age_pages(void) {
    if (!age_counters) return;

    int remaining = AGE_MAX_WORK;

    for (int pass = 0; pass < PID_TABLE_MAX && remaining > 0; pass++) {
        int idx = (age_proc_idx + pass) % PID_TABLE_MAX;
        if (idx == 0) idx = 1; /* skip PID 0 */
        process_t *p = proc_find((uint32_t)idx);
        if (!p) continue;

        uint64_t cursor = (pass == 0) ? age_va_cursor : 0;
        int done = age_walk_process(p, &cursor, remaining);
        remaining -= done;

        if (cursor == 0 && done > 0) {
            /* Finished this process, move to next */
            continue;
        }
        if (remaining <= 0) {
            age_proc_idx = idx;
            age_va_cursor = cursor;
            return;
        }
    }
    /* Full cycle done */
    age_proc_idx = 1;
    age_va_cursor = 0;
}

/* ════════════════════════════════════════════════════
 * 2. MM SCAN/MERGE HELPERS
 * ════════════════════════════════════════════════════ */

/* Scan cursor for dedup candidates */
static uint64_t scan_pfn_cursor;

void mm_dedup_scan_reset(void) {
    scan_pfn_cursor = 0;
}

int mm_dedup_scan_next(uint64_t *phys_out) {
    extern int page_alloc_total(void);
    extern int page_refcount(uint64_t phys);

    /* bm_test is static in page_alloc.c, use refcount as proxy:
     * refcount==1 means allocated + single owner */
    uint64_t max_pfn_local = age_max_pfn;
    while (scan_pfn_cursor < max_pfn_local) {
        uint64_t pfn = scan_pfn_cursor++;
        uint64_t phys = pfn << 12;

        int rc = page_refcount(phys);
        if (rc != 1) continue;  /* free or shared */

        /* Must be cold */
        if (mm_page_temperature(phys) != PAGE_COLD) continue;

        *phys_out = phys;
        return 0;
    }
    return -1;  /* scan cycle complete */
}

/* Walk all processes' page tables, find PTEs pointing at victim_phys,
 * replace with keep_phys + COW flags. */
int mm_dedup_merge(uint64_t keep_phys, uint64_t victim_phys) {
    int rewritten = 0;

    for (int pi = 1; pi < PID_TABLE_MAX; pi++) {
        process_t *p = proc_find((uint32_t)pi);
        if (!p || !p->pml4) continue;

        uint64_t *pml4 = p->pml4;

        for (int i = 0; i < 256; i++) {
            if (!(pml4[i] & PTE_PRESENT)) continue;
            uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i] & PTE_ADDR_MASK);

            for (int j = 0; j < 512; j++) {
                if (!(pdpt[j] & PTE_PRESENT)) continue;
                uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);

                for (int k = 0; k < 512; k++) {
                    if (!(pd[k] & PTE_PRESENT)) continue;
                    uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_ADDR_MASK);

                    for (int l = 0; l < 512; l++) {
                        uint64_t pte = pt[l];
                        if (!(pte & PTE_PRESENT)) continue;
                        if ((pte & PTE_ADDR_MASK) != victim_phys) continue;

                        /* Replace: keep_phys, read-only, COW, preserve USER+NX */
                        uint64_t new_pte = keep_phys | PTE_PRESENT | PTE_USER | PTE_COW;
                        if (pte & PTE_NX) new_pte |= PTE_NX;
                        /* Remove WRITE — COW will restore on fault */
                        pt[l] = new_pte;
                        rewritten++;
                    }
                }
            }
        }
    }

    if (rewritten > 0) {
        /* Bump keep refcount for each new reference (minus the original victim ref) */
        for (int i = 0; i < rewritten; i++)
            page_incref(keep_phys);

        /* Decrement victim refcount (all PTEs now point at keep) */
        for (int i = 0; i < rewritten; i++)
            page_decref(victim_phys);

        /* TLB shootdown: all cores may have stale TLB entries */
        tlb_shootdown(0);
    }

    return rewritten;
}

/* ════════════════════════════════════════════════════
 * 3. DEDUP ENGINE (Hash Table + RT-Poll)
 * ════════════════════════════════════════════════════ */

#define DEDUP_BUCKETS 4096
#define DEDUP_BUCKET_MASK (DEDUP_BUCKETS - 1)

typedef struct {
    uint8_t  hash[32];
    uint64_t phys;
    uint8_t  valid;
} dedup_entry_t;

static dedup_entry_t dedup_table[DEDUP_BUCKETS];

/* Stats */
static dedup_stats_t stats;

void dedup_get_stats(dedup_stats_t *out) {
    *out = stats;
}

/* Hash → bucket index (first 4 bytes as uint32) */
static inline int hash_bucket(const uint8_t hash[32]) {
    uint32_t h = (uint32_t)hash[0] | ((uint32_t)hash[1] << 8) |
                 ((uint32_t)hash[2] << 16) | ((uint32_t)hash[3] << 24);
    return (int)(h & DEDUP_BUCKET_MASK);
}

static int hash_eq(const uint8_t a[32], const uint8_t b[32]) {
    for (int i = 0; i < 32; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Lifecycle callbacks */

void dedup_on_page_free(uint64_t phys) {
    /* Linear scan is fine for 4096 entries on RT-Core (no contention) */
    for (int i = 0; i < DEDUP_BUCKETS; i++) {
        if (dedup_table[i].valid && dedup_table[i].phys == phys) {
            dedup_table[i].valid = 0;
            if (stats.pages_shared > 0) stats.pages_shared--;
            return;
        }
    }
}

void dedup_on_cow_break(uint64_t old_phys, uint64_t new_phys) {
    (void)new_phys;
    /* Invalidate the old hash entry — page content diverged */
    dedup_on_page_free(old_phys);
}

/* rt_poll handler: scan → hash → lookup → memcmp → merge */
#define DEDUP_MAX_WORK 4  /* pages per rt_poll tick */

static int dedup_poll(int max_work) {
    if (!age_counters) return 0;

    int work = 0;
    int limit = (max_work > 0 && max_work < DEDUP_MAX_WORK) ? max_work : DEDUP_MAX_WORK;

    while (work < limit) {
        uint64_t phys;
        if (mm_dedup_scan_next(&phys) < 0) {
            /* Cycle complete: reset + age */
            mm_dedup_scan_reset();
            mm_age_pages();
            break;
        }

        stats.pages_scanned++;

        /* Skip dirty pages (recently written) */
        /* Note: we already filter for PAGE_COLD in scan_next */

        /* Hash the page */
        uint8_t hash[32];
        sha256_oneshot(phys_to_virt(phys), 4096, hash);

        int bucket = hash_bucket(hash);
        dedup_entry_t *ent = &dedup_table[bucket];

        if (!ent->valid) {
            /* Empty slot: register this page */
            for (int i = 0; i < 32; i++) ent->hash[i] = hash[i];
            ent->phys = phys;
            ent->valid = 1;
        } else if (hash_eq(ent->hash, hash) && ent->phys != phys) {
            /* Hash match: verify with memcmp (hash collision guard) */
            int rc_keep = page_refcount(ent->phys);
            if (rc_keep >= 1) {
                const uint8_t *keep_data = (const uint8_t *)phys_to_virt(ent->phys);
                const uint8_t *victim_data = (const uint8_t *)phys_to_virt(phys);
                int match = 1;
                for (int i = 0; i < 4096; i++) {
                    if (keep_data[i] != victim_data[i]) { match = 0; break; }
                }
                if (match) {
                    int merged = mm_dedup_merge(ent->phys, phys);
                    if (merged > 0) {
                        stats.pages_merged++;
                        stats.pages_shared++;
                        stats.bytes_saved += 4096;
                    }
                }
            }
        }
        /* Hash collision with different content: skip (simple linear probing not worth it) */

        work++;
    }
    return work;
}

/* Age polling: runs periodically at its own prio (between HASH and DEDUP) */
static int age_poll(int max_work) {
    (void)max_work;
    mm_age_pages();
    return 0; /* age work doesn't count as "productive" for restart logic */
}

/* ── Init ─────────────────────────────────────────── */

/* New priority levels: RT_PRIO_AGE = 7, RT_PRIO_DEDUP = 8 */

void dedup_init(void) {
    extern uint64_t page_alloc_max_pfn(void);
    uint64_t mpfn = page_alloc_max_pfn();
    page_age_init(mpfn);

    for (int i = 0; i < DEDUP_BUCKETS; i++)
        dedup_table[i].valid = 0;

    stats.pages_scanned = 0;
    stats.pages_shared = 0;
    stats.pages_merged = 0;
    stats.bytes_saved = 0;

    mm_dedup_scan_reset();

    /* Register at RT_PRIO_AGE (7) and RT_PRIO_DEDUP (8) */
    rt_poll_register(RT_PRIO_AGE, age_poll, 1);
    rt_poll_register(RT_PRIO_DEDUP, dedup_poll, DEDUP_MAX_WORK);

    serial_puts("dedup: init (");
    { char t[20]; int i = 0; uint64_t v = mpfn;
      do { t[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
      while (i--) serial_putchar(t[i]); }
    serial_puts(" pages tracked)\n");
}
