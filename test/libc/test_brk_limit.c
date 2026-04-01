/* test: brk grows freely (RLIMIT_DATA = unlimited, like Linux default) */
#include "ktest.h"

static void test_brk_limit(void) {
    puts("\n[mm/brk-limit]\n");

    long cur = sc1(SYS_BRK, 0);
    check("brk readable", cur > 0);
    if (cur <= 0) return;

    /* brk should succeed for reasonable growth (RLIMIT_DATA = unlimited) */
    long grown = sc1(SYS_BRK, cur + 0x100000); /* +1MB */
    check("brk +1MB succeeds", grown >= cur + 0x100000);

    /* Touch the new memory to verify demand paging works */
    if (grown >= cur + 0x100000) {
        volatile char *p = (volatile char *)(cur + 0x1000);
        *p = 42;
        check_val("brk memory writable", *p, 42);
    }

    /* Shrink back */
    long shrunk = sc1(SYS_BRK, cur);
    check_val("brk shrink", shrunk, cur);

    /* brk must reject kernel-space addresses */
    long bad = sc1(SYS_BRK, 0xFFFF800000000000ULL);
    check_val("brk rejects kernel addr", bad, cur);

    /* brk must reject going below brk_base */
    long below = sc1(SYS_BRK, 0x1000);
    check_val("brk rejects below base", below, cur);
}

TEST("mm/brk-limit", test_brk_limit);
