#include "ktest.h"

static void test_mremap(void) {
    puts("\n[mremap]\n");

    /* mmap 4KB, write data, mremap grow to 8KB — data preserved */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mremap: initial mmap", addr > 0);
    if (addr <= 0) return;

    volatile char *p = (volatile char *)addr;
    p[0] = 0xAA;
    p[4095] = 0xBB;

    long grown = sc5(SYS_MREMAP, addr, 4096, 8192, MREMAP_MAYMOVE, 0);
    check("mremap: grow 4K->8K", grown > 0);
    if (grown > 0) {
        volatile char *g = (volatile char *)grown;
        check_val("mremap: data[0] preserved", (long)(unsigned char)g[0], 0xAA);
        check_val("mremap: data[4095] preserved", (long)(unsigned char)g[4095], 0xBB);

        /* mremap shrink 8KB -> 4KB — data in first 4KB preserved */
        long shrunk = sc5(SYS_MREMAP, grown, 8192, 4096, 0, 0);
        check_val("mremap: shrink returns same addr", shrunk, grown);
        if (shrunk > 0) {
            volatile char *s = (volatile char *)shrunk;
            check_val("mremap: shrink data[0]", (long)(unsigned char)s[0], 0xAA);
            check_val("mremap: shrink data[4095]", (long)(unsigned char)s[4095], 0xBB);
            sc2(SYS_MUNMAP, shrunk, 4096);
        }
    } else {
        /* Cleanup original if grow failed */
        sc2(SYS_MUNMAP, addr, 4096);
    }

    /* mremap without MAYMOVE when expansion blocked -> ENOMEM */
    long a1 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (a1 > 0) {
        /* Map something right after to block expansion */
        long a2 = sc6(SYS_MMAP, a1 + 4096, 4096, PROT_RW,
                       MAP_PRIV_ANON | MAP_FIXED, -1, 0);
        if (a2 > 0) {
            long fail_grow = sc5(SYS_MREMAP, a1, 4096, 8192, 0, 0);
            check_val("mremap: no MAYMOVE blocked -> ENOMEM", fail_grow, -ENOMEM);
            sc2(SYS_MUNMAP, a2, 4096);
        }
        sc2(SYS_MUNMAP, a1, 4096);
    }

    /* Same size -> returns same address */
    long a3 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (a3 > 0) {
        long same = sc5(SYS_MREMAP, a3, 4096, 4096, 0, 0);
        check_val("mremap: same size noop", same, a3);
        sc2(SYS_MUNMAP, a3, 4096);
    }
}

TEST("mremap", test_mremap);
