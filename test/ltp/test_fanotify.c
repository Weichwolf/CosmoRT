/* LTP fanotify02/04/07/08/11/12/fanotify_child — ported to ktest
 * CosmoRT returns -ENOSYS for fanotify_init (SYS_FANOTIFY_INIT=300).
 * All tests reduce to verifying that. */
#include "ktest.h"

/* ── fanotify02: directory children monitoring ── */

static void test_fanotify02(void) {
    puts("\n[ltp/fanotify02]\n");
    long r = sc2(SYS_FANOTIFY_INIT, 0, O_RDONLY);
    check_val("fanotify_init ENOSYS", r, -ENOSYS);
}

/* ── fanotify04: special flags (ONLYDIR, DONT_FOLLOW, FLUSH) ── */

static void test_fanotify04(void) {
    puts("\n[ltp/fanotify04]\n");
    long r = sc2(SYS_FANOTIFY_INIT, 0, O_RDONLY);
    check_val("fanotify_init ENOSYS", r, -ENOSYS);
}

/* ── fanotify07: permission events on instance destruction ── */

static void test_fanotify07(void) {
    puts("\n[ltp/fanotify07]\n");
    long r = sc2(SYS_FANOTIFY_INIT, 0, O_RDONLY);
    check_val("fanotify_init ENOSYS", r, -ENOSYS);
}

/* ── fanotify08: FAN_CLOEXEC flag ── */

static void test_fanotify08(void) {
    puts("\n[ltp/fanotify08]\n");
    long r = sc2(SYS_FANOTIFY_INIT, 0, O_RDONLY);
    check_val("fanotify_init ENOSYS", r, -ENOSYS);
}

/* ── fanotify11: FAN_REPORT_TID ── */

static void test_fanotify11(void) {
    puts("\n[ltp/fanotify11]\n");
    long r = sc2(SYS_FANOTIFY_INIT, 0, O_RDONLY);
    check_val("fanotify_init ENOSYS", r, -ENOSYS);
}

/* ── fanotify12: FAN_OPEN_EXEC ── */

static void test_fanotify12(void) {
    puts("\n[ltp/fanotify12]\n");
    long r = sc2(SYS_FANOTIFY_INIT, 0, O_RDONLY);
    check_val("fanotify_init ENOSYS", r, -ENOSYS);
}

/* ── fanotify_mark on invalid fd ── */

static void test_fanotify_mark(void) {
    puts("\n[ltp/fanotify_mark]\n");
    /* SYS_FANOTIFY_MARK = 301 */
    long r = sc5(SYS_FANOTIFY_MARK, -1, 0, 0, 0, 0);
    /* Either ENOSYS (not implemented) or EBADF (bad fd) */
    check("fanotify_mark returns error", r < 0);
}

TEST("ltp/fanotify02",    test_fanotify02);
TEST("ltp/fanotify04",    test_fanotify04);
TEST("ltp/fanotify07",    test_fanotify07);
TEST("ltp/fanotify08",    test_fanotify08);
TEST("ltp/fanotify11",    test_fanotify11);
TEST("ltp/fanotify12",    test_fanotify12);
TEST("ltp/fanotify_mark", test_fanotify_mark);
