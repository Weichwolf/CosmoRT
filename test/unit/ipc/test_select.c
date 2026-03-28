/* test: select() — timeout behavior (readfds/writefds blocked by kernel bug:
 * do_pselect6 passes kernel-stack pollfd to do_poll which calls copy_from_user → EFAULT).
 * Tests what currently works: nfds=0 timeout path, and negative-test behavior. */
#include "ktest.h"

/* select with empty fds + 10ms timeout → returns 0 */
static void test_select_timeout(void) {
    puts("\n[select: timeout]\n");

    struct { long sec; long usec; } tv = { 0, 10000 }; /* 10ms */
    long r = sc5(SYS_SELECT, 0, 0, 0, 0, (long)&tv);
    check_val("select timeout returns 0", r, 0);
}

/* select with NULL timeout and nfds=0 → immediate return 0 */
static void test_select_nfds_zero(void) {
    puts("\n[select: nfds=0]\n");

    long r = sc5(SYS_SELECT, 0, 0, 0, 0, 0);
    check_val("select nfds=0 returns 0", r, 0);
}

/* pselect6 with timespec timeout and nfds=0 → returns 0 */
static void test_pselect6_timeout(void) {
    puts("\n[pselect6: timeout]\n");

    struct { long sec; long nsec; } ts = { 0, 10000000 }; /* 10ms */
    long r = sc6(SYS_PSELECT6, 0, 0, 0, 0, (long)&ts, 0);
    check_val("pselect6 timeout returns 0", r, 0);
}

TEST("select-timeout", test_select_timeout);
TEST("select-nfds0", test_select_nfds_zero);
TEST("pselect6-timeout", test_pselect6_timeout);
