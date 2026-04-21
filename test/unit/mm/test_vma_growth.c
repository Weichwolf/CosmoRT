/* Class A: Pool-Growth-Pfade - VMA / mmap */
#include "ktest.h"

#define VMA_COUNT       128
#define VMA_PAGE_SIZE   4096

#define MANY_MMAP_COUNT 500

static void test_vma_many_distinct(void) {
    puts("\n[VMA_MANY_DISTINCT]\n");

    long base = sc6(SYS_MMAP, 0, (long)VMA_COUNT * 2 * VMA_PAGE_SIZE,
                    PROT_NONE, MAP_PRIV_ANON, -1, 0);
    check("reserve region", base > 0);
    if (base <= 0) return;

    int mapped = 0;
    for (int i = 0; i < VMA_COUNT; i++) {
        long addr = base + (long)i * 2 * VMA_PAGE_SIZE;
        long r = sc6(SYS_MMAP, addr, VMA_PAGE_SIZE, PROT_RW,
                     MAP_PRIV_ANON | MAP_FIXED, -1, 0);
        if (r != addr) break;
        *(volatile uint64_t *)addr = (uint64_t)i;
        mapped++;
    }
    check_ge("distinct VMAs mapped", mapped, VMA_COUNT);

    int verified = 0;
    for (int i = 0; i < mapped; i++) {
        long addr = base + (long)i * 2 * VMA_PAGE_SIZE;
        if (*(volatile uint64_t *)addr == (uint64_t)i) verified++;
    }
    check_val("content unchanged after many VMAs", verified, mapped);

    sc2(SYS_MUNMAP, base, (long)VMA_COUNT * 2 * VMA_PAGE_SIZE);
}

static void test_mmap_random_unmap(void) {
    puts("\n[MMAP_RANDOM_UNMAP]\n");

    static long addrs[MANY_MMAP_COUNT];
    int alloced = 0;
    for (int i = 0; i < MANY_MMAP_COUNT; i++) {
        long r = sc6(SYS_MMAP, 0, VMA_PAGE_SIZE, PROT_RW,
                     MAP_PRIV_ANON, -1, 0);
        if (r <= 0) break;
        *(volatile uint32_t *)r = (uint32_t)i;
        addrs[i] = r;
        alloced++;
    }
    check_ge("mmap many anon pages", alloced, 256);

    uint32_t seed = 0xdeadbeef;
    int freed = 0;
    for (int step = 0; step < alloced; step++) {
        seed = seed * 1103515245u + 12345u;
        int idx = (int)(seed % (uint32_t)alloced);
        if (!addrs[idx]) continue;
        if (sc2(SYS_MUNMAP, addrs[idx], VMA_PAGE_SIZE) == 0) freed++;
        addrs[idx] = 0;
    }
    for (int i = 0; i < alloced; i++) {
        if (addrs[i]) { sc2(SYS_MUNMAP, addrs[i], VMA_PAGE_SIZE); freed++; }
    }
    check_val("random-order munmap frees all", freed, alloced);

    long probe = sc6(SYS_MMAP, 0, VMA_PAGE_SIZE, PROT_RW,
                     MAP_PRIV_ANON, -1, 0);
    check("post-storm mmap succeeds", probe > 0);
    if (probe > 0) sc2(SYS_MUNMAP, probe, VMA_PAGE_SIZE);
}

TEST("vma/many_distinct",     test_vma_many_distinct);
TEST("vma/random_unmap",      test_mmap_random_unmap);
