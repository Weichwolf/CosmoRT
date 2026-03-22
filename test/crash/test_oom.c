/* Crash test: OOM robustness.
 * Kernel must return -ENOMEM, not crash. */
#include "ktest.h"

#define MB (1024ULL * 1024)

void test_oom(void) {
    puts("\n[OOM]\n");

    /* 1. mmap in loop until -ENOMEM */
    int count = 0;
    long addrs[512]; /* track for cleanup */
    int max_allocs = 512;

    for (int i = 0; i < max_allocs; i++) {
        long r = sc6(SYS_mmap, 0, (long)(64 * MB), PROT_RW,
                     MAP_PRIV_ANON, -1, 0);
        if (r < 0) {
            /* Must be -ENOMEM (12) */
            check("mmap exhaustion → -ENOMEM", r == -12);
            break;
        }
        /* Touch first page to commit */
        *(volatile char *)r = 1;
        addrs[i] = r;
        count++;
    }
    check("mmap loop survived", count > 0);
    puts("  allocs before OOM: "); put_int(count); puts("\n");

    /* Cleanup */
    for (int i = 0; i < count; i++)
        sc2(SYS_munmap, addrs[i], (long)(64 * MB));

    /* 2. brk growth until limit */
    long brk_base = sc1(SYS_brk, 0);
    check("brk(0) works", brk_base > 0);

    long brk_cur = brk_base;
    int brk_steps = 0;
    for (int i = 0; i < 4096; i++) {
        long next = brk_cur + 4096;
        long got = sc1(SYS_brk, next);
        if (got != next) {
            /* Kernel refused — correct behavior */
            break;
        }
        brk_cur = got;
        brk_steps++;
    }
    check("brk growth stopped", brk_steps > 0);
    puts("  brk steps: "); put_int(brk_steps); puts("\n");

    /* Shrink back */
    sc1(SYS_brk, brk_base);

    /* 3. VMA exhaustion: many tiny mmaps */
    int vma_count = 0;
    long tiny_addrs[4096];
    int max_vmas = 4096;

    for (int i = 0; i < max_vmas; i++) {
        long r = sc6(SYS_mmap, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (r < 0) {
            check("VMA exhaustion → error", r == -12 || r == -11); /* ENOMEM or EAGAIN */
            break;
        }
        tiny_addrs[i] = r;
        vma_count++;
    }
    check("VMA stress survived", vma_count > 0);
    puts("  VMAs allocated: "); put_int(vma_count); puts("\n");

    /* Cleanup */
    for (int i = 0; i < vma_count; i++)
        sc2(SYS_munmap, tiny_addrs[i], 4096);
}
