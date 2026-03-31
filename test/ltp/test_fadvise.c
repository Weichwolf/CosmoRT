/* LTP posix_fadvise01-04 — ported to ktest
 * SYS_FADVISE64 = 221: fadvise64(fd, offset, len, advice)
 * posix_fadvise advice values (Linux x86_64): */
#include "ktest.h"

#define FADV_NORMAL     0
#define FADV_RANDOM     1
#define FADV_SEQUENTIAL 2
#define FADV_WILLNEED   3
#define FADV_DONTNEED   4
#define FADV_NOREUSE    5

/* ── fadvise01: valid advice values return 0 ── */

static void test_fadvise01(void) {
    puts("\n[ltp/fadvise01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fadvise_test", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    int advices[] = { FADV_NORMAL, FADV_SEQUENTIAL, FADV_RANDOM,
                      FADV_NOREUSE, FADV_WILLNEED, FADV_DONTNEED };

    for (int i = 0; i < 6; i++) {
        long r = sc4(SYS_FADVISE64, fd, 0, 0, advices[i]);
        check_val("fadvise64 returns 0", r, 0);
    }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fadvise_test");
}

/* ── fadvise02: EBADF for invalid fd ── */

static void test_fadvise02(void) {
    puts("\n[ltp/fadvise02]\n");

    int advices[] = { FADV_NORMAL, FADV_SEQUENTIAL, FADV_RANDOM,
                      FADV_NOREUSE, FADV_WILLNEED, FADV_DONTNEED };

    for (int i = 0; i < 6; i++) {
        long r = sc4(SYS_FADVISE64, 9999, 0, 0, advices[i]);
        /* fadvise64 returns error number directly (positive), not -errno.
         * Linux kernel: returns -EBADF which libc converts. At syscall
         * level we get -EBADF. */
        check_val("fadvise64 bad fd", r, -EBADF);
    }
}

/* ── fadvise03: EINVAL for invalid advice values ── */

static void test_fadvise03(void) {
    puts("\n[ltp/fadvise03]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fadvise_test3", O_RDONLY | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Advice values 6..31 that are not valid */
    int invalid[] = { 6, 7, 8, 9, 10, 15, 31 };
    for (int i = 0; i < 7; i++) {
        long r = sc4(SYS_FADVISE64, fd, 0, 0, invalid[i]);
        check_val("fadvise64 invalid advice EINVAL", r, -EINVAL);
    }

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fadvise_test3");
}

/* ── fadvise04: ESPIPE for pipe fd ── */

static void test_fadvise04(void) {
    puts("\n[ltp/fadvise04]\n");

    long pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    int advices[] = { FADV_NORMAL, FADV_SEQUENTIAL, FADV_RANDOM,
                      FADV_NOREUSE, FADV_WILLNEED, FADV_DONTNEED };

    for (int i = 0; i < 6; i++) {
        long ret = sc4(SYS_FADVISE64, pipefd[0], 0, 0, advices[i]);
        check_val("fadvise64 pipe ESPIPE", ret, -ESPIPE);
    }

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

TEST("ltp/fadvise01", test_fadvise01);
TEST("ltp/fadvise02", test_fadvise02);
TEST("ltp/fadvise03", test_fadvise03);
TEST("ltp/fadvise04", test_fadvise04);
