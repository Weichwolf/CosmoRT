/* LTP close01/close02/close_range02 — ported to ktest */
#include "ktest.h"

#define CLOSE_RANGE_CLOEXEC (1U << 2)

/* ── close01: basic close of file, pipe, socket ── */

static void test_close01_file(void) {
    puts("\n[ltp/close01-file]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/close01_file", O_RDWR | O_CREAT, 0700);
    check("open file", fd >= 0);
    if (fd < 0) return;

    long r = sc1(SYS_CLOSE, fd);
    check_val("close file", r, 0);

    /* Verify fd is actually closed */
    r = sc2(SYS_FCNTL, fd, F_GETFD);
    check_val("fd closed EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/close01_file");
}

static void test_close01_pipe(void) {
    puts("\n[ltp/close01-pipe]\n");

    int pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    r = sc1(SYS_CLOSE, pipefd[0]);
    check_val("close pipe read", r, 0);

    r = sc1(SYS_CLOSE, pipefd[1]);
    check_val("close pipe write", r, 0);
}

static void test_close01_socket(void) {
    puts("\n[ltp/close01-socket]\n");

    long fd = sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    check("socket", fd >= 0);
    if (fd < 0) return;

    long r = sc1(SYS_CLOSE, fd);
    check_val("close socket", r, 0);
}

/* ── close02: EBADF for invalid fd ── */

static void test_close02(void) {
    puts("\n[ltp/close02]\n");

    long r = sc1(SYS_CLOSE, -1);
    check_val("close(-1) EBADF", r, -EBADF);

    /* Double close — second should EBADF */
    long fd = sc3(SYS_OPEN, (long)"/tmp/close02_file", O_RDWR | O_CREAT, 0700);
    check("open", fd >= 0);
    if (fd < 0) return;

    r = sc1(SYS_CLOSE, fd);
    check_val("first close", r, 0);

    r = sc1(SYS_CLOSE, fd);
    check_val("double close EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/close02_file");
}

/* ── close_range02: basic close_range and error cases ── */

/* close_range on valid range */
static void test_close_range_basic(void) {
    puts("\n[ltp/close_range-basic]\n");

    long fd1 = sc3(SYS_OPEN, (long)"/tmp/cr_f1", O_RDWR | O_CREAT, 0700);
    check("open f1", fd1 >= 0);
    if (fd1 < 0) return;

    long fd2 = sc1(SYS_DUP, fd1);
    check("dup", fd2 >= 0);
    if (fd2 < 0) { sc1(SYS_CLOSE, fd1); return; }

    /* close_range(fd1, fd2, 0) — should close both */
    long lo = fd1 < fd2 ? fd1 : fd2;
    long hi = fd1 > fd2 ? fd1 : fd2;
    long r = sc3(SYS_CLOSE_RANGE, lo, hi, 0);
    check_val("close_range", r, 0);

    /* Verify both are closed */
    r = sc2(SYS_FCNTL, fd1, F_GETFD);
    check_val("fd1 closed", r, -EBADF);
    r = sc2(SYS_FCNTL, fd2, F_GETFD);
    check_val("fd2 closed", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/cr_f1");
}

/* close_range EINVAL when min > max */
static void test_close_range_einval(void) {
    puts("\n[ltp/close_range-einval]\n");

    long r = sc3(SYS_CLOSE_RANGE, 4, 3, 0);
    check_val("close_range(4,3) EINVAL", r, -EINVAL);
}

/* close_range EINVAL for invalid flags */
static void test_close_range_bad_flags(void) {
    puts("\n[ltp/close_range-bad-flags]\n");

    long r = sc3(SYS_CLOSE_RANGE, 3, (long)(~0U), (long)(~0U));
    check_val("close_range bad flags EINVAL", r, -EINVAL);
}

/* close_range with large lower fd (no fds in range — should succeed) */
static void test_close_range_high(void) {
    puts("\n[ltp/close_range-high]\n");

    long r = sc3(SYS_CLOSE_RANGE, (long)(~0U), (long)(~0U), 0);
    check_val("close_range(UINT_MAX, UINT_MAX)", r, 0);
}

/* close_range with CLOSE_RANGE_CLOEXEC */
static void test_close_range_cloexec(void) {
    puts("\n[ltp/close_range-cloexec]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/cr_cloexec", O_RDWR | O_CREAT, 0700);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_CLOSE_RANGE, fd, fd, CLOSE_RANGE_CLOEXEC);
    if (r == -EINVAL) {
        /* CLOSE_RANGE_CLOEXEC not supported */
        puts("  SKIP  CLOSE_RANGE_CLOEXEC not supported\n");
        sc1(SYS_CLOSE, fd);
        sc1(SYS_UNLINK, (long)"/tmp/cr_cloexec");
        return;
    }
    check_val("close_range CLOEXEC", r, 0);

    /* fd should still be open but have FD_CLOEXEC */
    long flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("fd still open", flags >= 0);
    check("FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/cr_cloexec");
}

TEST("ltp/close01-file",          test_close01_file);
TEST("ltp/close01-pipe",          test_close01_pipe);
TEST("ltp/close01-socket",        test_close01_socket);
TEST("ltp/close02",               test_close02);
TEST("ltp/close_range-basic",     test_close_range_basic);
TEST("ltp/close_range-einval",    test_close_range_einval);
TEST("ltp/close_range-bad-flags", test_close_range_bad_flags);
TEST("ltp/close_range-high",      test_close_range_high);
TEST("ltp/close_range-cloexec",   test_close_range_cloexec);
