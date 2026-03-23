/* Crash test: V8 Pointer Compression cage pattern.
 * Allocate 4.5GB PROT_NONE, find 4GB boundary, trim, activate, JIT. */
#include "ktest.h"

#define GB (1024ULL * 1024 * 1024)
#define MB (1024ULL * 1024)

static void test_v8_cage(void) {
    puts("\n[V8 Cage]\n");

    /* 1. Reserve 4.5GB PROT_NONE */
    uint64_t reserve_sz = 4 * GB + 512 * MB;
    long base = sc6(SYS_MMAP, 0, (long)reserve_sz, PROT_NONE,
                    MAP_PRIV_ANON, -1, 0);
    if (base < 0) {
        /* Kernel may reject 4.5GB on a 4GB-RAM machine — acceptable */
        check("mmap 4.5GB PROT_NONE (or -ENOMEM)", base == -12);
        return;
    }
    check("mmap 4.5GB PROT_NONE", base > 0x1000);

    /* 2. Find 4GB-aligned boundary */
    uint64_t aligned = ((uint64_t)base + (4 * GB - 1)) & ~(4 * GB - 1);

    /* 3. Trim below */
    if (aligned > (uint64_t)base) {
        long r = sc2(SYS_MUNMAP, base, (long)(aligned - (uint64_t)base));
        check("munmap below boundary", r == 0);
    }

    /* Trim above: keep 512MB from aligned boundary */
    uint64_t cage_sz = 512 * MB;
    uint64_t cage_end = aligned + cage_sz;
    uint64_t alloc_end = (uint64_t)base + reserve_sz;
    if (cage_end < alloc_end) {
        long r = sc2(SYS_MUNMAP, (long)cage_end, (long)(alloc_end - cage_end));
        check("munmap above cage", r == 0);
    }

    /* 4. Activate first 64KB as RW */
    long mp = sc3(SYS_MPROTECT, (long)aligned, 65536, PROT_RW);
    check("mprotect 64KB RW", mp == 0);

    /* 5. Write test pattern */
    volatile uint64_t *p = (volatile uint64_t *)aligned;
    for (int i = 0; i < 8192 / 8; i++)
        p[i] = 0xCAFEBABE00000000ULL | (uint64_t)i;

    /* 6. Read back and verify */
    int corrupt = 0;
    for (int i = 0; i < 8192 / 8; i++) {
        if (p[i] != (0xCAFEBABE00000000ULL | (uint64_t)i)) { corrupt = 1; break; }
    }
    check("cage data integrity", !corrupt);

    /* 7. Activate a code page (JIT pattern) */
    uint64_t code_off = 65536; /* right after the data region */
    long mp2 = sc3(SYS_MPROTECT, (long)(aligned + code_off), 4096, PROT_RW);
    check("mprotect code page RW", mp2 == 0);

    uint8_t *cp = (uint8_t *)(aligned + code_off);
    cp[0] = 0xb8; cp[1] = 99; cp[2] = 0; cp[3] = 0; cp[4] = 0; /* mov eax, 99 */
    cp[5] = 0xc3; /* ret */

    long mp3 = sc3(SYS_MPROTECT, (long)(aligned + code_off), 4096,
                   PROT_READ | PROT_EXEC);
    check("mprotect code page RX", mp3 == 0);

    /* 8. Execute JIT code */
    int (*fn)(void) = (int (*)(void))(aligned + code_off);
    int result = fn();
    check("JIT exec returns 99", result == 99);

    /* 9. Cleanup */
    long mu = sc2(SYS_MUNMAP, (long)aligned, (long)cage_sz);
    check("munmap cage", mu == 0);
}

CRASH_TEST("crash/v8_cage", test_v8_cage);
