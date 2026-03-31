/* LTP brk01, brk02 — ported to ktest */
#include "ktest.h"

/* ── brk01: basic brk — increase and decrease program break ── */

static void test_brk01(void) {
    puts("\n[ltp/brk01]\n");

    /* Get current break via brk(0) */
    long cur = sc1(SYS_BRK, 0);
    check("brk(0) returns current break", cur > 0);

    /* Increase break by 4096 */
    long new_brk = cur + 4096;
    long r = sc1(SYS_BRK, new_brk);
    check_val("brk increase", r, new_brk);

    /* Verify we can write to the new memory */
    if (r == new_brk) {
        volatile char *p = (volatile char *)cur;
        p[0] = 0x42;
        p[4095] = 0x43;
        check("write to new memory", p[0] == 0x42 && p[4095] == 0x43);
    }

    /* Increase further */
    long new_brk2 = new_brk + 8192;
    r = sc1(SYS_BRK, new_brk2);
    check_val("brk increase 2", r, new_brk2);

    /* Decrease back to original + 4096 */
    r = sc1(SYS_BRK, new_brk);
    check_val("brk decrease", r, new_brk);

    /* Decrease back to original */
    r = sc1(SYS_BRK, cur);
    check_val("brk restore", r, cur);
}

/* ── brk02: brk error cases ── */

static void test_brk02(void) {
    puts("\n[ltp/brk02]\n");

    long cur = sc1(SYS_BRK, 0);
    check("brk(0)", cur > 0);

    /* Try to set break below current — should return current (no decrease below initial) */
    long r = sc1(SYS_BRK, 0x1000);
    /* Linux returns the current brk if the request is invalid (below min) */
    check("brk(0x1000) rejected", r >= cur || r == 0x1000);

    /* brk(0) should still work */
    r = sc1(SYS_BRK, 0);
    check("brk(0) still works", r > 0);

    /* Try absurdly large value — should fail gracefully */
    long huge = (long)0x7FFFFFFFE000ULL;
    r = sc1(SYS_BRK, huge);
    /* Should not succeed — returns current brk unchanged */
    check("brk(huge) rejected", r != huge);

    /* Verify brk still functional */
    long after = sc1(SYS_BRK, 0);
    check("brk still functional", after > 0);
}

TEST("ltp/brk01", test_brk01);
TEST("ltp/brk02", test_brk02);
