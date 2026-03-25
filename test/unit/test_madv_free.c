#include "ktest.h"

static void test_madv_free(void) {
    puts("\n[MADV_FREE]\n");

    /* Basic: mmap -> write -> MADV_FREE -> read -> data still there */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("madv_free: mmap", addr > 0);
    if (addr > 0) {
        volatile unsigned char *p = (volatile unsigned char *)addr;
        p[0] = 0xAA;
        p[100] = 0xBB;
        long r = sc3(SYS_MADVISE, addr, 4096, MADV_FREE);
        check_val("madv_free: returns 0", r, 0);
        /* Data should still be there (no memory pressure) */
        check_val("madv_free: data[0] preserved", (long)p[0], 0xAA);
        check_val("madv_free: data[100] preserved", (long)p[100], 0xBB);
        sc2(SYS_MUNMAP, addr, 4096);
    }

    /* Re-write after MADV_FREE: new data sticks (page rescued) */
    addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (addr > 0) {
        volatile unsigned char *p = (volatile unsigned char *)addr;
        p[0] = 0x11;
        long r = sc3(SYS_MADVISE, addr, 4096, MADV_FREE);
        check_val("madv_free rewrite: advise ok", r, 0);
        /* Write new data — this sets Dirty, protecting page from reclaim */
        p[0] = 0x22;
        p[4095] = 0x33;
        check_val("madv_free rewrite: data[0]", (long)p[0], 0x22);
        check_val("madv_free rewrite: data[4095]", (long)p[4095], 0x33);
        sc2(SYS_MUNMAP, addr, 4096);
    }

    /* Multiple ranges: some rewritten, some not */
    long a1 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long a2 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (a1 > 0 && a2 > 0) {
        volatile unsigned char *p1 = (volatile unsigned char *)a1;
        volatile unsigned char *p2 = (volatile unsigned char *)a2;
        p1[0] = 0xCC;
        p2[0] = 0xDD;
        sc3(SYS_MADVISE, a1, 4096, MADV_FREE);
        sc3(SYS_MADVISE, a2, 4096, MADV_FREE);
        /* Rewrite only a1 */
        p1[0] = 0xEE;
        check_val("madv_free multi: rewritten", (long)p1[0], 0xEE);
        /* a2 still readable (no pressure) */
        check_val("madv_free multi: untouched", (long)p2[0], 0xDD);
    }
    if (a1 > 0) sc2(SYS_MUNMAP, a1, 4096);
    if (a2 > 0) sc2(SYS_MUNMAP, a2, 4096);

    /* Large range: 1MB MADV_FREE */
    long big = sc6(SYS_MMAP, 0, 1024*1024, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (big > 0) {
        volatile unsigned char *p = (volatile unsigned char *)big;
        /* Touch every page */
        for (int i = 0; i < 256; i++)
            p[i * 4096] = (unsigned char)(i & 0xFF);
        long r = sc3(SYS_MADVISE, big, 1024*1024, MADV_FREE);
        check_val("madv_free large: advise ok", r, 0);
        /* Spot check some pages still readable */
        check_val("madv_free large: page 0", (long)p[0], 0);
        check_val("madv_free large: page 100", (long)p[100*4096], 100);
        sc2(SYS_MUNMAP, big, 1024*1024);
    }

    /* Double MADV_FREE: no crash */
    addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (addr > 0) {
        volatile unsigned char *p = (volatile unsigned char *)addr;
        p[0] = 0x42;
        long r1 = sc3(SYS_MADVISE, addr, 4096, MADV_FREE);
        long r2 = sc3(SYS_MADVISE, addr, 4096, MADV_FREE);
        check_val("madv_free double: first", r1, 0);
        check_val("madv_free double: second", r2, 0);
        sc2(SYS_MUNMAP, addr, 4096);
    }

    /* Partial overlap: two MADV_FREE on overlapping ranges */
    long over = sc6(SYS_MMAP, 0, 3*4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (over > 0) {
        volatile unsigned char *p = (volatile unsigned char *)over;
        p[0] = 0x10;
        p[4096] = 0x20;
        p[8192] = 0x30;
        /* First: pages 0-1 */
        long r1 = sc3(SYS_MADVISE, over, 2*4096, MADV_FREE);
        /* Second: pages 1-2 (overlaps page 1) */
        long r2 = sc3(SYS_MADVISE, over + 4096, 2*4096, MADV_FREE);
        check_val("madv_free overlap: first", r1, 0);
        check_val("madv_free overlap: second", r2, 0);
        /* All pages still readable */
        check_val("madv_free overlap: p0", (long)p[0], 0x10);
        check_val("madv_free overlap: p1", (long)p[4096], 0x20);
        check_val("madv_free overlap: p2", (long)p[8192], 0x30);
        sc2(SYS_MUNMAP, over, 3*4096);
    }

    /* MADV_FREE + mprotect: mark free, change prot, change back */
    addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (addr > 0) {
        volatile unsigned char *p = (volatile unsigned char *)addr;
        p[0] = 0x77;
        sc3(SYS_MADVISE, addr, 4096, MADV_FREE);
        /* mprotect to PROT_NONE then back to RW */
        sc3(SYS_MPROTECT, addr, 4096, PROT_NONE);
        sc3(SYS_MPROTECT, addr, 4096, PROT_RW);
        /* Page may or may not still have data — just verify no crash */
        volatile unsigned char val = p[0];
        (void)val;
        check("madv_free mprotect: no crash", 1);
        sc2(SYS_MUNMAP, addr, 4096);
    }

    /* Return value: MADV_FREE on invalid address -> ENOMEM */
    long bad = sc3(SYS_MADVISE, 0x7FF000000000ULL, 4096, MADV_FREE);
    check_val("madv_free invalid addr: ENOMEM", bad, -ENOMEM);

    /* Stress: alloc many pages, MADV_FREE half, alloc more -> reclaim works */
    long stress_base = sc6(SYS_MMAP, 0, 64*4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (stress_base > 0) {
        volatile unsigned char *p = (volatile unsigned char *)stress_base;
        /* Touch all 64 pages */
        for (int i = 0; i < 64; i++)
            p[i * 4096] = (unsigned char)i;
        /* MADV_FREE the first 32 pages */
        long r = sc3(SYS_MADVISE, stress_base, 32*4096, MADV_FREE);
        check_val("madv_free stress: advise ok", r, 0);
        /* The second 32 pages should be unaffected */
        check_val("madv_free stress: unaffected[32]", (long)p[32*4096], 32);
        check_val("madv_free stress: unaffected[63]", (long)p[63*4096], 63);
        sc2(SYS_MUNMAP, stress_base, 64*4096);
    }

    /* MADV_FREE + fork: parent marks free, fork, child sees data */
    addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (addr > 0) {
        volatile unsigned char *p = (volatile unsigned char *)addr;
        p[0] = 0x55;
        sc3(SYS_MADVISE, addr, 4096, MADV_FREE);
        long pid = sc5(SYS_CLONE, 17 /* SIGCHLD */, 0, 0, 0, 0);
        if (pid == 0) {
            /* Child: check data is visible */
            volatile unsigned char *cp = (volatile unsigned char *)addr;
            if (cp[0] != 0x55)
                sc1(SYS_EXIT_GROUP, 1);
            sc1(SYS_EXIT_GROUP, 0);
        } else if (pid > 0) {
            /* Parent: wait for child */
            int status = 0;
            sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
            /* Child exit code 0 = data was visible */
            check_val("madv_free fork: child saw data", (status >> 8) & 0xFF, 0);
        }
        sc2(SYS_MUNMAP, addr, 4096);
    }
}

TEST("madv_free", test_madv_free);
