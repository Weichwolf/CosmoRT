#include "ktest.h"

/* ── MAP_SHARED test ─────────────────────────── */

#define MAP_SHARED_FLAG  0x01

static void test_map_shared(void) {
    puts("\n[MAP_SHARED]\n");

    /* mmap with MAP_SHARED | MAP_ANONYMOUS should succeed */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW,
                    MAP_SHARED_FLAG | MAP_ANONYMOUS, -1, 0);
    check("mmap MAP_SHARED succeeds", addr > 0);
    if (addr > 0) {
        volatile char *p = (volatile char *)addr;
        p[0] = 0x55;
        check_val("MAP_SHARED page writable", (long)p[0], 0x55);
        sc2(SYS_MUNMAP, addr, 4096);
    }
}

TEST("MAP_SHARED", test_map_shared);
