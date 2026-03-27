#include "ktest.h"

static void test_memory(void) {
    puts("\n[Memory]\n");

    /* mmap anonymous */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap anon succeeds", addr > 0);
    if (addr > 0) {
        volatile char *p = (volatile char *)addr;
        p[0] = 0x42;
        check_val("mmap page writable", (long)p[0], 0x42);
        /* Page was zeroed */
        check_val("mmap page zeroed", (long)p[1], 0);
        long r = sc2(SYS_MUNMAP, addr, 4096);
        check_val("munmap returns 0", r, 0);
    }

    /* mmap large (demand paging) */
    long big = sc6(SYS_MMAP, 0, 1024*1024, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 1MB succeeds", big > 0);
    if (big > 0) {
        volatile char *p = (volatile char *)big;
        /* Touch first and last page (triggers page faults) */
        p[0] = 1;
        p[1024*1024 - 1] = 2;
        check_val("demand page first", (long)p[0], 1);
        check_val("demand page last", (long)p[1024*1024-1], 2);
        sc2(SYS_MUNMAP, big, 1024*1024);
    }

    /* brk */
    long brk0 = sc1(SYS_BRK, 0);
    check("brk(0) returns current", brk0 > 0);
    long brk1 = sc1(SYS_BRK, brk0 + 4096);
    check_val("brk grow", brk1, brk0 + 4096);

    /* brk shrink: pages freed, re-grow gives zeroed pages */
    if (brk1 > 0) {
        volatile char *bp = (volatile char *)brk0;
        bp[0] = 0xCC;
        bp[4095] = 0xDD;
        /* Shrink back */
        long brk2 = sc1(SYS_BRK, brk0);
        check_val("brk shrink", brk2, brk0);
        /* Grow again — should get fresh zero pages */
        long brk3 = sc1(SYS_BRK, brk0 + 4096);
        check_val("brk re-grow", brk3, brk0 + 4096);
        volatile char *bp2 = (volatile char *)brk0;
        check_val("brk shrink+grow zeroed[0]", (long)(unsigned char)bp2[0], 0);
        check_val("brk shrink+grow zeroed[4095]", (long)(unsigned char)bp2[4095], 0);
    }
}

TEST("memory", test_memory);
