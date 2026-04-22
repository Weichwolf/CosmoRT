/* LTP fdatasync01-02 — ported to ktest */
#include "ktest.h"

/* ── fdatasync01: basic fdatasync on a writable file ── */

static void test_fdatasync01(void) {
    puts("\n[ltp/fdatasync01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fdatasync01", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("open", fd >= 0);
    if (fd < 0) return;

    sc3(SYS_WRITE, fd, (long)"test data", 9);

    long r = sc1(SYS_FDATASYNC, fd);
    check_val("fdatasync", r, 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fdatasync01");
}

/* ── fdatasync02: EBADF for invalid fd ── */

static void test_fdatasync02_ebadf(void) {
    puts("\n[ltp/fdatasync02-ebadf]\n");

    long r = sc1(SYS_FDATASYNC, -1);
    check_val("fdatasync(-1) EBADF", r, -EBADF);

    r = sc1(SYS_FDATASYNC, 9999);
    check_val("fdatasync(9999) EBADF", r, -EBADF);
}

/* ── fdatasync02: EINVAL for special file (/dev/null equiv — pipe read end) ── */

static void test_fdatasync02_einval(void) {
    puts("\n[ltp/fdatasync02-einval]\n");

    int pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    r = sc1(SYS_FDATASYNC, pipefd[0]);
    check_val("fdatasync(pipe) EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── fdatasync02: EINVAL fuer /dev/null (file_op ohne ->fsync) ── */

static void test_fdatasync02_devnull(void) {
    puts("\n[ltp/fdatasync02-devnull]\n");

    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open /dev/null", fd >= 0);
    if (fd < 0) return;

    long r = sc1(SYS_FDATASYNC, fd);
    check_val("fdatasync(/dev/null) EINVAL", r, -EINVAL);

    r = sc1(SYS_FSYNC, fd);
    check_val("fsync(/dev/null) EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fd);
}

TEST("ltp/fdatasync01",         test_fdatasync01);
TEST("ltp/fdatasync02-ebadf",   test_fdatasync02_ebadf);
TEST("ltp/fdatasync02-einval",  test_fdatasync02_einval);
TEST("ltp/fdatasync02-devnull", test_fdatasync02_devnull);
