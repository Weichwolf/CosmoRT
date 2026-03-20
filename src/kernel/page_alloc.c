/* CosmoRT Page Allocator — bitmap-based physical page allocator
 *
 * Each bit in the bitmap = one 4KB page.
 * 0 = free, 1 = allocated.
 * Max 128MB heap = 32768 pages = 4KB bitmap.
 */

#include "page_alloc.h"
#include "serial.h"
#include "spinlock.h"
#include "memops.h"

#define MAX_PAGES (128 * 1024 * 1024 / 4096) /* 32768 */
#define BITMAP_BYTES (MAX_PAGES / 8)          /* 4096 */

static uint8_t bitmap[BITMAP_BYTES];
static uint8_t *heap_base;
static spinlock_t page_lock = SPINLOCK_INIT;
static int heap_pages;
static int alloc_count;

void page_alloc_init(uint8_t *base, size_t size) {
    heap_base = base;
    heap_pages = (int)(size / 4096);
    if (heap_pages > MAX_PAGES) heap_pages = MAX_PAGES;
    alloc_count = 0;

    for (int i = 0; i < BITMAP_BYTES; i++) bitmap[i] = 0;
    /* Mark pages beyond heap as allocated */
    for (int i = heap_pages; i < MAX_PAGES; i++)
        bitmap[i / 8] |= (uint8_t)(1 << (i % 8));

    serial_puts("page_alloc: ");
    char t[12]; int ti = 0;
    int v = heap_pages;
    do { t[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" pages (");
    v = heap_pages * 4 / 1024;
    ti = 0;
    do { t[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" MB)\n");
}

void *page_alloc(void) {
    uint64_t flags;
    spin_lock_irq(&page_lock, &flags);

    for (int i = 0; i < heap_pages; i++) {
        int byte = i / 8, bit = i % 8;
        if (!(bitmap[byte] & (1 << bit))) {
            bitmap[byte] |= (uint8_t)(1 << bit);
            alloc_count++;
            spin_unlock_irq(&page_lock, flags);

            uint8_t *p = heap_base + (uint64_t)i * 4096;
            page_zero(p);
            return p;
        }
    }

    spin_unlock_irq(&page_lock, flags);
    serial_puts("page_alloc: OOM\n");
    return (void *)0;
}

void page_free(void *page) {
    if (!page) return;
    uint64_t flags;
    spin_lock_irq(&page_lock, &flags);
    uint64_t offset = (uint8_t *)page - heap_base;
    int idx = (int)(offset / 4096);
    if (idx >= 0 && idx < heap_pages) {
        bitmap[idx / 8] &= (uint8_t)~(1 << (idx % 8));
        alloc_count--;
    }
    spin_unlock_irq(&page_lock, flags);
}

void *pages_alloc(int n) {
    if (n <= 0) return (void *)0;
    for (int i = 0; i <= heap_pages - n; i++) {
        int ok = 1;
        for (int j = 0; j < n; j++) {
            if (bitmap[(i + j) / 8] & (1 << ((i + j) % 8))) {
                ok = 0;
                i += j; /* skip ahead */
                break;
            }
        }
        if (ok) {
            for (int j = 0; j < n; j++)
                bitmap[(i + j) / 8] |= (uint8_t)(1 << ((i + j) % 8));
            alloc_count += n;
            uint8_t *p = heap_base + (uint64_t)i * 4096;
            pages_zero(p, n);
            return p;
        }
    }
    serial_puts("pages_alloc: OOM\n");
    return (void *)0;
}

void pages_free(void *base, int n) {
    if (!base) return;
    uint64_t offset = (uint8_t *)base - heap_base;
    int idx = (int)(offset / 4096);
    for (int j = 0; j < n && idx + j < heap_pages; j++) {
        bitmap[(idx + j) / 8] &= (uint8_t)~(1 << ((idx + j) % 8));
        alloc_count--;
    }
}

int page_alloc_total(void) { return heap_pages; }
int page_alloc_free(void)  { return heap_pages - alloc_count; }
