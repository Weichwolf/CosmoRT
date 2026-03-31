/* test: brk VMA overlap check — reproduces musl malloc-brk-fail */
#include "ktest.h"

/*
 * brk() must fail when address space is exhausted.
 * CosmoRT: brk succeeds even when mmap has filled all available space,
 * allowing heap to collide with existing mappings.
 */
static void test_brk_limit(void) {
    puts("\n[mm/brk-limit]\n");

    /* Fill address space with 1MB MAP_PRIVATE|MAP_ANONYMOUS mappings */
    long sz = 1024L * 1024;
    int count = 0;
    for (;;) {
        long r = sc6(SYS_MMAP, 0, sz, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (r < 0) break;
        count++;
        /* Safety: don't loop forever if kernel has huge address space */
        if (count > 4096) break;
    }
    puts("  mmap fill: "); put_int(count); puts(" x 1MB\n");
    check("filled some space", count > 0);

    /* Get current brk */
    long cur_brk = sc1(SYS_BRK, 0);
    check("brk readable", cur_brk > 0);

    /* Try to extend brk by 10000 — should fail (no space) */
    long new_brk = sc1(SYS_BRK, cur_brk + 10000);

    puts("  cur_brk=0x"); put_hex((uint64_t)cur_brk);
    puts("  new_brk=0x"); put_hex((uint64_t)new_brk); puts("\n");

    /* On failure, brk returns the old value unchanged */
    /* Bug: brk succeeds and returns cur_brk+10000 despite no space */
    check_val("brk should fail", new_brk, cur_brk);
}

TEST("mm/brk-limit", test_brk_limit);
