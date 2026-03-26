#include "ktest.h"

#define HUGE_SIZE (2ULL * 1024 * 1024)  /* 2MB */

/* Basic: mmap 2MB aligned → write → data correct */
static void test_huge_basic(void) {
    puts("\n[THP basic]\n");

    long addr = sc6(SYS_MMAP, 0, HUGE_SIZE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 2MB", addr > 0x1000);

    /* Write first and last page */
    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0xDEADBEEF;
    volatile uint64_t *last = (volatile uint64_t *)(addr + HUGE_SIZE - 8);
    *last = 0xCAFEBABE;

    check_val("first page", (long)*p, (long)0xDEADBEEF);
    check_val("last page", (long)*last, (long)0xCAFEBABE);

    sc2(SYS_MUNMAP, addr, HUGE_SIZE);
}

/* Large mapping: mmap 4MB → write across all pages */
static void test_huge_4mb(void) {
    puts("\n[THP 4MB]\n");

    long addr = sc6(SYS_MMAP, 0, 2 * HUGE_SIZE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 4MB", addr > 0x1000);

    /* Touch every 4KB page */
    for (uint64_t off = 0; off < 2 * HUGE_SIZE; off += 4096) {
        volatile uint64_t *p = (volatile uint64_t *)(addr + off);
        *p = off;
    }

    /* Verify */
    int ok = 1;
    for (uint64_t off = 0; off < 2 * HUGE_SIZE; off += 4096) {
        volatile uint64_t *p = (volatile uint64_t *)(addr + off);
        if (*p != off) { ok = 0; break; }
    }
    check("all pages correct", ok);

    sc2(SYS_MUNMAP, addr, 2 * HUGE_SIZE);
}

/* Fallback: small mapping stays at 4KB */
static void test_huge_fallback_small(void) {
    puts("\n[THP fallback small]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 4KB", addr > 0x1000);

    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 42;
    check_val("4KB write ok", (long)*p, 42);

    sc2(SYS_MUNMAP, addr, 4096);
}

/* COW split: fork → child writes on huge page → data isolated */
static void test_huge_cow_split(void) {
    puts("\n[THP COW split]\n");

    long addr = sc6(SYS_MMAP, 0, HUGE_SIZE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 2MB", addr > 0x1000);

    /* Touch first and middle page before fork */
    volatile uint64_t *first = (volatile uint64_t *)addr;
    volatile uint64_t *mid   = (volatile uint64_t *)(addr + HUGE_SIZE / 2);
    *first = 0x1111;
    *mid   = 0x2222;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);

    if (pid == 0) {
        /* Child: verify parent values, then overwrite */
        if (*first != 0x1111) sc1(SYS_EXIT_GROUP, 1);
        if (*mid   != 0x2222) sc1(SYS_EXIT_GROUP, 2);
        *first = 0x3333;
        *mid   = 0x4444;
        if (*first != 0x3333) sc1(SYS_EXIT_GROUP, 3);
        if (*mid   != 0x4444) sc1(SYS_EXIT_GROUP, 4);
        sc1(SYS_EXIT_GROUP, 0);
    } else {
        int status = 0;
        sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        check("child exited 0", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);
        check_val("parent first unchanged", (long)*first, (long)0x1111);
        check_val("parent mid unchanged", (long)*mid, (long)0x2222);
        sc2(SYS_MUNMAP, addr, HUGE_SIZE);
    }
}

/* Partial munmap: unmap middle 4KB of a 2MB region */
static void test_huge_partial_munmap(void) {
    puts("\n[THP partial munmap]\n");

    long addr = sc6(SYS_MMAP, 0, HUGE_SIZE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 2MB", addr > 0x1000);

    volatile uint64_t *first = (volatile uint64_t *)addr;
    volatile uint64_t *mid   = (volatile uint64_t *)(addr + HUGE_SIZE / 2);
    volatile uint64_t *last  = (volatile uint64_t *)(addr + HUGE_SIZE - 8);
    *first = 0xAAAA;
    *mid   = 0xBBBB;
    *last  = 0xCCCC;

    /* Unmap one 4KB page in the middle */
    long ret = sc2(SYS_MUNMAP, addr + HUGE_SIZE / 2, 4096);
    check("partial munmap", ret == 0);

    /* First and last should still be accessible */
    check_val("first ok", (long)*first, (long)0xAAAA);
    check_val("last ok", (long)*last, (long)0xCCCC);

    sc2(SYS_MUNMAP, addr, HUGE_SIZE);
}

/* mprotect: change protection on part of a huge page */
static void test_huge_mprotect(void) {
    puts("\n[THP mprotect]\n");

    long addr = sc6(SYS_MMAP, 0, HUGE_SIZE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 2MB", addr > 0x1000);

    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0x5555;

    /* mprotect first 4KB to read-only */
    long ret = sc3(SYS_MPROTECT, addr, 4096, PROT_READ);
    check("mprotect first 4KB", ret == 0);

    /* Second page should still be writable */
    volatile uint64_t *p2 = (volatile uint64_t *)(addr + 4096);
    *p2 = 0x6666;
    check_val("second page write ok", (long)*p2, (long)0x6666);

    /* First page should still be readable */
    check_val("first page read ok", (long)*p, (long)0x5555);

    sc2(SYS_MUNMAP, addr, HUGE_SIZE);
}

/* Stress: many 2MB allocations */
static void test_huge_stress(void) {
    puts("\n[THP stress]\n");

    #define NALLOC 8
    long addrs[NALLOC];
    int ok = 1;

    for (int i = 0; i < NALLOC; i++) {
        addrs[i] = sc6(SYS_MMAP, 0, HUGE_SIZE, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (addrs[i] <= 0x1000) { ok = 0; break; }
        /* Write pattern */
        volatile uint64_t *p = (volatile uint64_t *)addrs[i];
        *p = (uint64_t)(0xAA00 + i);
    }
    check("all allocs succeeded", ok);

    /* Verify patterns */
    for (int i = 0; i < NALLOC && ok; i++) {
        volatile uint64_t *p = (volatile uint64_t *)addrs[i];
        if (*p != (uint64_t)(0xAA00 + i)) ok = 0;
    }
    check("all patterns correct", ok);

    for (int i = 0; i < NALLOC; i++)
        if (addrs[i] > 0x1000) sc2(SYS_MUNMAP, addrs[i], HUGE_SIZE);
    #undef NALLOC
}

/* Alignment: non-aligned mmap stays functional */
static void test_huge_alignment(void) {
    puts("\n[THP alignment]\n");

    /* Mmap an odd size — should still work (may or may not get huge pages) */
    long addr = sc6(SYS_MMAP, 0, HUGE_SIZE + 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap odd size", addr > 0x1000);

    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0x7777;
    volatile uint64_t *last = (volatile uint64_t *)(addr + HUGE_SIZE);
    *last = 0x8888;
    check_val("first ok", (long)*p, (long)0x7777);
    check_val("past 2MB ok", (long)*last, (long)0x8888);

    sc2(SYS_MUNMAP, addr, HUGE_SIZE + 4096);
}

TEST("thp_basic", test_huge_basic);
TEST("thp_4mb", test_huge_4mb);
TEST("thp_fallback_small", test_huge_fallback_small);
TEST("thp_cow_split", test_huge_cow_split);
TEST("thp_partial_munmap", test_huge_partial_munmap);
TEST("thp_mprotect", test_huge_mprotect);
TEST("thp_stress", test_huge_stress);
TEST("thp_alignment", test_huge_alignment);
