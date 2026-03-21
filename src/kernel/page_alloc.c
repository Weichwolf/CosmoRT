/* CosmoRT Buddy Allocator — power-of-2 free lists, O(1) single-page alloc
 *
 * Orders: 0=4KB, 1=8KB, ..., 9=2MB. MAX_ORDER=9 matches x86_64 huge page.
 * Free blocks stored in-place (first 8 bytes of free page = next pointer).
 * Bitmap tracks allocated pages for buddy merging.
 * Bitmap itself lives in first usable UEFI region — no static limits.
 */

#include "page_alloc.h"
#include "serial.h"
#include "spinlock.h"
#include "memops.h"
#include "config.h"

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1ULL << PAGE_SHIFT)
#define MAX_ORDER   9   /* 2^9 = 512 pages = 2MB — x86_64 huge page boundary */

/* 8GB direct-map cap (entry.asm maps PML4[256..263]) */
#define DIRECT_MAP_LIMIT (8ULL * 1024 * 1024 * 1024)

struct free_block {
    struct free_block *next;
};

static struct free_block *free_lists[MAX_ORDER + 1];
static spinlock_t buddy_lock = SPINLOCK_INIT;

/* Bitmap: one bit per page frame, set = allocated, clear = free */
static uint64_t *buddy_bitmap;
static uint64_t bitmap_qwords;
static uint64_t max_pfn;

/* Stats */
static uint64_t total_pages;
static uint64_t alloc_count;

/* --- Bitmap ops --- */

static inline void bm_set(uint64_t pfn) {
    buddy_bitmap[pfn / 64] |= (1ULL << (pfn % 64));
}

static inline void bm_clear(uint64_t pfn) {
    buddy_bitmap[pfn / 64] &= ~(1ULL << (pfn % 64));
}

static inline int bm_test(uint64_t pfn) {
    return (int)((buddy_bitmap[pfn / 64] >> (pfn % 64)) & 1);
}

/* --- Internal alloc/free (caller holds buddy_lock) --- */

static void *buddy_alloc_order(int order) {
    for (int o = order; o <= MAX_ORDER; o++) {
        if (!free_lists[o]) continue;

        struct free_block *blk = free_lists[o];
        free_lists[o] = blk->next;

        /* Split down to requested order */
        uint64_t phys = virt_to_phys(blk);
        while (o > order) {
            o--;
            uint64_t buddy_phys = phys + (1ULL << (PAGE_SHIFT + o));
            struct free_block *buddy = (struct free_block *)phys_to_virt(buddy_phys);
            buddy->next = free_lists[o];
            free_lists[o] = buddy;
        }

        /* Mark allocated in bitmap */
        uint64_t pfn = phys >> PAGE_SHIFT;
        uint64_t npages = 1ULL << order;
        for (uint64_t i = 0; i < npages; i++) bm_set(pfn + i);
        alloc_count += npages;

        return phys_to_virt(phys);
    }
    return (void *)0; /* OOM */
}

static void buddy_free_order(void *ptr, int order) {
    uint64_t phys = virt_to_phys(ptr);
    uint64_t pfn = phys >> PAGE_SHIFT;
    uint64_t npages = 1ULL << order;

    /* Clear bitmap */
    for (uint64_t i = 0; i < npages; i++) bm_clear(pfn + i);
    alloc_count -= npages;

    /* Merge with buddy while possible */
    while (order < MAX_ORDER) {
        uint64_t buddy_phys = phys ^ (1ULL << (PAGE_SHIFT + order));
        uint64_t buddy_pfn = buddy_phys >> PAGE_SHIFT;
        uint64_t buddy_npages = 1ULL << order;

        /* Buddy must be within tracked range */
        if (buddy_pfn + buddy_npages > max_pfn) break;

        /* Check buddy is completely free */
        int buddy_free = 1;
        for (uint64_t i = 0; i < buddy_npages; i++) {
            if (bm_test(buddy_pfn + i)) { buddy_free = 0; break; }
        }
        if (!buddy_free) break;

        /* Remove buddy from its free list */
        struct free_block *buddy = (struct free_block *)phys_to_virt(buddy_phys);
        struct free_block **pp = &free_lists[order];
        int found = 0;
        while (*pp) {
            if (*pp == buddy) { *pp = buddy->next; found = 1; break; }
            pp = &(*pp)->next;
        }
        if (!found) break; /* buddy not in free list — don't merge */

        /* Merge: take lower address */
        if (buddy_phys < phys) phys = buddy_phys;
        order++;
    }

    struct free_block *blk = (struct free_block *)phys_to_virt(phys);
    blk->next = free_lists[order];
    free_lists[order] = blk;
}

/* --- Compute order for n pages --- */

static int order_for_pages(int n) {
    int order = 0;
    while ((1 << order) < n) order++;
    return order;
}

/* --- Helper: print decimal to serial --- */

static void serial_dec(uint64_t v) {
    char t[20]; int i = 0;
    do { t[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

/* --- Add one region of pages to the buddy system --- */

static void add_region(uint64_t phys_start, uint64_t phys_end) {
    /* Align start up and end down to page boundary */
    phys_start = (phys_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    phys_end &= ~(PAGE_SIZE - 1);
    if (phys_end <= phys_start) return;

    uint64_t addr = phys_start;
    while (addr < phys_end) {
        /* Find largest order block that:
         * 1. Is naturally aligned (addr is aligned to block size)
         * 2. Fits within the remaining region */
        int order = 0;
        while (order < MAX_ORDER) {
            uint64_t block_size = 1ULL << (PAGE_SHIFT + order + 1);
            if (addr & (block_size - 1)) break;  /* not aligned */
            if (addr + block_size > phys_end) break; /* doesn't fit */
            order++;
        }

        /* Add block at this order */
        struct free_block *blk = (struct free_block *)phys_to_virt(addr);
        blk->next = free_lists[order];
        free_lists[order] = blk;

        uint64_t npages = 1ULL << order;
        total_pages += npages;
        /* Bitmap bits are already clear (= free) from zeroing */

        addr += npages << PAGE_SHIFT;
    }
}

/* --- Public API --- */

void page_alloc_init(uint8_t *base, size_t size) {
    /* Compat stub — real init is page_alloc_add_uefi_regions */
    (void)base; (void)size;
}

void page_alloc_add_uefi_regions(void *mmap_virt, uint64_t mmap_size,
                                  uint64_t desc_size) {
    uint8_t *mmap = (uint8_t *)mmap_virt;
    uint64_t count = mmap_size / desc_size;
    int truncated = 0;

    /* Pass 1: find highest physical address within direct-map limit */
    max_pfn = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        if (type != 7) continue; /* EfiConventionalMemory only */
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        uint64_t end = phys + pages * PAGE_SIZE;
        if (end > DIRECT_MAP_LIMIT) {
            end = DIRECT_MAP_LIMIT;
            truncated = 1;
        }
        uint64_t pfn = end >> PAGE_SHIFT;
        if (pfn > max_pfn) max_pfn = pfn;
    }

    if (max_pfn == 0) {
        serial_puts("page_alloc: no usable memory!\n");
        return;
    }

    /* Bitmap size: one bit per PFN, rounded up to 8 bytes */
    bitmap_qwords = (max_pfn + 63) / 64;
    uint64_t bitmap_bytes = bitmap_qwords * 8;
    uint64_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;

    /* Pass 2: find first region large enough for bitmap */
    buddy_bitmap = (uint64_t *)0;
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        if (type != 7) continue;
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        uint64_t region_size = pages * PAGE_SIZE;
        if (phys < 0x100000) continue; /* skip low memory */
        uint64_t end = phys + region_size;
        if (end > DIRECT_MAP_LIMIT) end = DIRECT_MAP_LIMIT;
        if (end <= phys) continue;
        if (end - phys >= bitmap_bytes) {
            bitmap_phys = phys;
            buddy_bitmap = (uint64_t *)phys_to_virt(phys);
            break;
        }
    }

    if (!buddy_bitmap) {
        serial_puts("page_alloc: no region for bitmap!\n");
        return;
    }

    /* Zero the bitmap (all bits clear = all free) */
    kmemset(buddy_bitmap, 0, bitmap_bytes);

    /* Mark ALL pages as allocated initially (set all bits).
     * add_region will clear bits for pages it adds. Actually, no —
     * add_region leaves bits clear (free). But we need non-usable pages
     * to be marked allocated. Simpler: set entire bitmap, then clear
     * as we add regions. */
    kmemset(buddy_bitmap, 0xFF, bitmap_bytes);

    /* Init free lists */
    for (int o = 0; o <= MAX_ORDER; o++) free_lists[o] = (struct free_block *)0;
    total_pages = 0;
    alloc_count = 0;

    /* Pass 3: add all usable regions (skip bitmap area) */
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        if (type != 7) continue; /* EfiConventionalMemory */
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        uint64_t end = phys + pages * PAGE_SIZE;

        if (phys < 0x100000) continue; /* skip low memory */
        if (end > DIRECT_MAP_LIMIT) end = DIRECT_MAP_LIMIT;
        if (end <= phys) continue;

        /* Skip bitmap area within this region */
        uint64_t bm_end = bitmap_phys + bitmap_pages * PAGE_SIZE;
        if (phys >= bitmap_phys && phys < bm_end) {
            /* Region starts inside bitmap — skip bitmap part */
            phys = bm_end;
        } else if (phys < bitmap_phys && end > bitmap_phys) {
            /* Bitmap is in the middle of this region — split */
            /* Clear bits and add the part before bitmap */
            uint64_t pfn_start = phys >> PAGE_SHIFT;
            uint64_t pfn_end = bitmap_phys >> PAGE_SHIFT;
            for (uint64_t p = pfn_start; p < pfn_end; p++) bm_clear(p);
            add_region(phys, bitmap_phys);
            /* Continue with part after bitmap */
            phys = bm_end;
        }

        if (end <= phys) continue;

        /* Clear bitmap bits for this region (mark as free) */
        uint64_t pfn_start = phys >> PAGE_SHIFT;
        uint64_t pfn_end = end >> PAGE_SHIFT;
        for (uint64_t p = pfn_start; p < pfn_end; p++) bm_clear(p);

        add_region(phys, end);
    }

    /* Mark all free pages as allocated in bitmap for consistency:
     * bitmap bit set = allocated. But we just cleared them.
     * The free lists contain free blocks. The bitmap says "free".
     * On alloc, we set bits. On free, we clear bits. Correct. */

    serial_puts("buddy: ");
    serial_dec(total_pages);
    serial_puts(" pages (");
    serial_dec(total_pages * 4 / 1024);
    serial_puts(" MB), bitmap ");
    serial_dec(bitmap_pages);
    serial_puts(" pages\n");

    /* Order distribution */
    serial_puts("buddy: orders");
    for (int o = 0; o <= MAX_ORDER; o++) {
        int cnt = 0;
        for (struct free_block *b = free_lists[o]; b; b = b->next) cnt++;
        if (cnt > 0) {
            serial_puts(" ["); serial_dec((uint64_t)o);
            serial_puts("]="); serial_dec((uint64_t)cnt);
        }
    }
    serial_putchar('\n');

    if (truncated)
        serial_puts("buddy: WARNING — RAM above 8GB truncated (direct-map limit)\n");
}

void *page_alloc(void) {
    uint64_t flags;
    spin_lock_irq(&buddy_lock, &flags);
    void *p = buddy_alloc_order(0);
    spin_unlock_irq(&buddy_lock, flags);
    if (p) page_zero(p);
    return p;
}

void page_free(void *page) {
    if (!page) return;
    uint64_t phys = virt_to_phys(page);
    uint64_t pfn = phys >> PAGE_SHIFT;
    if (pfn >= max_pfn) {
        serial_puts("page_free: pfn out of range! phys=");
        serial_hex64(phys);
        serial_puts("\n");
        return;
    }
    uint64_t flags;
    spin_lock_irq(&buddy_lock, &flags);
    buddy_free_order(page, 0);
    spin_unlock_irq(&buddy_lock, flags);
}

void *pages_alloc(int n) {
    if (n <= 0) return (void *)0;
    int order = order_for_pages(n);
    if (order > MAX_ORDER) {
        serial_puts("pages_alloc: order too large\n");
        return (void *)0;
    }
    uint64_t flags;
    spin_lock_irq(&buddy_lock, &flags);
    void *p = buddy_alloc_order(order);
    spin_unlock_irq(&buddy_lock, flags);
    if (p) pages_zero(p, 1 << order);
    return p;
}

void pages_free(void *base, int n) {
    if (!base || n <= 0) return;
    int order = order_for_pages(n);
    uint64_t flags;
    spin_lock_irq(&buddy_lock, &flags);
    buddy_free_order(base, order);
    spin_unlock_irq(&buddy_lock, flags);
}

int page_alloc_total(void) { return (int)total_pages; }
int page_alloc_free(void)  { return (int)(total_pages - alloc_count); }
