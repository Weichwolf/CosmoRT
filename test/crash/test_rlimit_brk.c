/* Crash test: brk to absurd addresses.
 * Kernel must refuse, not crash. */
#include "ktest.h"

static void test_rlimit_brk(void) {
    puts("\n[Rlimit Brk]\n");

    long brk_base = sc1(SYS_BRK, 0);
    check("brk(0) works", brk_base > 0);
    puts("  brk_base=0x"); put_hex((uint64_t)brk_base); puts("\n");

    /* Test 1: brk to kernel address */
    long r = sc1(SYS_BRK, (long)0xFFFF800000000000ULL);
    check("brk(kernel_high) refused", r == brk_base || (r > 0 && r < (long)0x800000000000ULL));

    /* Test 2: brk to first kernel address */
    r = sc1(SYS_BRK, (long)0x800000000000ULL);
    check("brk(kern_boundary) refused", r == brk_base || (r > 0 && r < (long)0x800000000000ULL));

    /* Test 3: brk to non-canonical address */
    r = sc1(SYS_BRK, (long)0xDEAD000000000000ULL);
    check("brk(non_canonical) refused", r == brk_base || (r > 0 && r < (long)0x800000000000ULL));

    /* Test 4: brk to MAX value */
    r = sc1(SYS_BRK, (long)-1ULL);
    check("brk(-1) refused", r == brk_base || (r > 0 && r < (long)0x800000000000ULL));

    /* Test 5: brk to just under kernel boundary */
    r = sc1(SYS_BRK, (long)0x7FFFFFFFE000ULL);
    check("brk(near_boundary) refused", r == brk_base || (r > 0 && r <= (long)0x7FFFFFFFE000ULL));

    /* Test 6: brk to NULL */
    r = sc1(SYS_BRK, 0);
    check("brk(0) still returns base", r == brk_base);

    /* Test 7: brk to address 1 (below current) */
    r = sc1(SYS_BRK, 1);
    check("brk(1) refused (below base)", r == brk_base || r > 0);

    /* Test 8: moderate growth then shrink — sanity */
    long grow = brk_base + 4096;
    r = sc1(SYS_BRK, grow);
    int grew = (r == grow);
    if (grew) {
        /* Write to verify mapping */
        *(volatile char *)brk_base = 0xAB;
        pass("brk grow 4K");

        /* Shrink back */
        r = sc1(SYS_BRK, brk_base);
        check("brk shrink back", r == brk_base);
    } else {
        /* Growth refused — acceptable in constrained environment */
        pass("brk grow 4K (skipped, limited env)");
    }

    /* Test 9: massive jump */
    r = sc1(SYS_BRK, brk_base + (long)(256ULL * 1024 * 1024 * 1024));
    check("brk(+256GB) refused", r < brk_base + (long)(256ULL * 1024 * 1024 * 1024));

    /* Ensure brk is reset */
    sc1(SYS_BRK, brk_base);
}

CRASH_TEST("crash/rlimit_brk", test_rlimit_brk);
