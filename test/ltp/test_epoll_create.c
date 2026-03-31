/* LTP epoll_create01/epoll_create02/epoll_create1_01/epoll_create1_02 — ported to ktest */
#include "ktest.h"

#define EPOLL_CLOEXEC   02000000
#define EPOLLPRI        0x002
#define EPOLLONESHOT    (1U << 30)

/* ── epoll_create01: size > 0 returns valid fd ── */

static void test_epoll_create01_size1(void) {
    puts("\n[ltp/epoll_create01-size1]\n");
    long fd = sc1(SYS_EPOLL_CREATE, 1);
    check("epoll_create(1) returns valid fd", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);
}

static void test_epoll_create01_sizemax(void) {
    puts("\n[ltp/epoll_create01-sizemax]\n");
    long fd = sc1(SYS_EPOLL_CREATE, 0x7fffffff);
    check("epoll_create(INT_MAX) returns valid fd", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);
}

/* ── epoll_create02: size <= 0 fails with EINVAL ── */

static void test_epoll_create02_zero(void) {
    puts("\n[ltp/epoll_create02-zero]\n");
    long r = sc1(SYS_EPOLL_CREATE, 0);
    check_val("epoll_create(0) EINVAL", r, -EINVAL);
}

static void test_epoll_create02_neg(void) {
    puts("\n[ltp/epoll_create02-neg]\n");
    long r = sc1(SYS_EPOLL_CREATE, -1);
    check_val("epoll_create(-1) EINVAL", r, -EINVAL);
}

/* ── epoll_create1_01: EPOLL_CLOEXEC flag ── */

static void test_epoll_create1_01_nocloexec(void) {
    puts("\n[ltp/epoll_create1_01-nocloexec]\n");
    long fd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1(0) returns valid fd", fd >= 0);
    if (fd < 0) return;

    long flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("FD_CLOEXEC not set", (flags & FD_CLOEXEC) == 0);
    sc1(SYS_CLOSE, fd);
}

static void test_epoll_create1_01_cloexec(void) {
    puts("\n[ltp/epoll_create1_01-cloexec]\n");
    long fd = sc1(SYS_EPOLL_CREATE1, EPOLL_CLOEXEC);
    check("epoll_create1(EPOLL_CLOEXEC) returns valid fd", fd >= 0);
    if (fd < 0) return;

    long flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);
    sc1(SYS_CLOSE, fd);
}

/* ── epoll_create1_02: invalid flags fail with EINVAL ── */

static void test_epoll_create1_02_neg1(void) {
    puts("\n[ltp/epoll_create1_02-neg1]\n");
    long r = sc1(SYS_EPOLL_CREATE1, -1);
    check_val("epoll_create1(-1) EINVAL", r, -EINVAL);
}

static void test_epoll_create1_02_bad(void) {
    puts("\n[ltp/epoll_create1_02-bad]\n");
    /* EPOLL_CLOEXEC + 1 is invalid */
    long r = sc1(SYS_EPOLL_CREATE1, EPOLL_CLOEXEC + 1);
    check_val("epoll_create1(EPOLL_CLOEXEC+1) EINVAL", r, -EINVAL);
}

TEST("ltp/epoll_create01-size1",       test_epoll_create01_size1);
TEST("ltp/epoll_create01-sizemax",     test_epoll_create01_sizemax);
TEST("ltp/epoll_create02-zero",        test_epoll_create02_zero);
TEST("ltp/epoll_create02-neg",         test_epoll_create02_neg);
TEST("ltp/epoll_create1_01-nocloexec", test_epoll_create1_01_nocloexec);
TEST("ltp/epoll_create1_01-cloexec",   test_epoll_create1_01_cloexec);
TEST("ltp/epoll_create1_02-neg1",      test_epoll_create1_02_neg1);
TEST("ltp/epoll_create1_02-bad",       test_epoll_create1_02_bad);
