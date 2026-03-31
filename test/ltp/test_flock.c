/* LTP flock01-04,06,07 — ported to ktest */
#include "ktest.h"

#define LOCK_SH     1
#define LOCK_EX     2
#define LOCK_NB     4
#define LOCK_UN     8
#define EWOULDBLOCK EAGAIN

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── flock01: basic SH/UN/EX locking ── */

static void test_flock01(void) {
    puts("\n[ltp/flock01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/flock01", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc2(SYS_FLOCK, fd, LOCK_SH);
    check_val("LOCK_SH", r, 0);

    r = sc2(SYS_FLOCK, fd, LOCK_UN);
    check_val("LOCK_UN", r, 0);

    r = sc2(SYS_FLOCK, fd, LOCK_EX);
    check_val("LOCK_EX", r, 0);

    r = sc2(SYS_FLOCK, fd, LOCK_UN);
    check_val("LOCK_UN after EX", r, 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/flock01");
}

/* ── flock02: error conditions ── */

static void test_flock02(void) {
    puts("\n[ltp/flock02]\n");

    /* EBADF */
    long r = sc2(SYS_FLOCK, -1, LOCK_SH);
    check_val("flock(-1) EBADF", r, -EBADF);

    long fd = sc3(SYS_OPEN, (long)"/tmp/flock02", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* EINVAL: LOCK_NB alone */
    r = sc2(SYS_FLOCK, fd, LOCK_NB);
    check_val("LOCK_NB alone EINVAL", r, -EINVAL);

    /* EINVAL: LOCK_SH | LOCK_EX */
    r = sc2(SYS_FLOCK, fd, LOCK_SH | LOCK_EX);
    check_val("SH|EX EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/flock02");
}

/* ── flock03: child cannot lock EX file locked by parent ── */

static void test_flock03(void) {
    puts("\n[ltp/flock03]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/flock03", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc2(SYS_FLOCK, fd, LOCK_EX);
    check_val("parent EX lock", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* child: try non-blocking exclusive lock — should fail */
        long cfd = sc3(SYS_OPEN, (long)"/tmp/flock03", O_RDWR, 0);
        long cr = sc2(SYS_FLOCK, cfd, LOCK_EX | LOCK_NB);
        /* exit 0 if EWOULDBLOCK, 1 otherwise */
        sc1(SYS_EXIT, (cr == -EWOULDBLOCK) ? 0 : 1);
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child got EWOULDBLOCK", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    sc2(SYS_FLOCK, fd, LOCK_UN);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/flock03");
}

/* ── flock04: shared vs exclusive lock combinations ── */

static void test_flock04(void) {
    puts("\n[ltp/flock04]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/flock04", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Parent holds SH lock */
    long r = sc2(SYS_FLOCK, fd, LOCK_SH);
    check_val("parent SH", r, 0);

    /* Child: SH|NB should succeed, EX|NB should fail */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/flock04", O_RDWR, 0);
        long cr = sc2(SYS_FLOCK, cfd, LOCK_SH | LOCK_NB);
        if (cr != 0) sc1(SYS_EXIT, 1);
        sc2(SYS_FLOCK, cfd, LOCK_UN);
        cr = sc2(SYS_FLOCK, cfd, LOCK_EX | LOCK_NB);
        if (cr != -EWOULDBLOCK) sc1(SYS_EXIT, 2);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT, 0);
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child SH ok, EX blocked", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Parent holds EX lock */
    sc2(SYS_FLOCK, fd, LOCK_UN);
    r = sc2(SYS_FLOCK, fd, LOCK_EX);
    check_val("parent EX", r, 0);

    pid = sc0(SYS_FORK);
    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/flock04", O_RDWR, 0);
        long cr = sc2(SYS_FLOCK, cfd, LOCK_SH | LOCK_NB);
        if (cr != -EWOULDBLOCK) sc1(SYS_EXIT, 1);
        cr = sc2(SYS_FLOCK, cfd, LOCK_EX | LOCK_NB);
        if (cr != -EWOULDBLOCK) sc1(SYS_EXIT, 2);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT, 0);
    }

    status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child SH+EX both blocked", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    sc2(SYS_FLOCK, fd, LOCK_UN);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/flock04");
}

/* ── flock06: two fds on same file — lock conflict ── */

static void test_flock06(void) {
    puts("\n[ltp/flock06]\n");

    long fd1 = sc3(SYS_OPEN, (long)"/tmp/flock06", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("open fd1", fd1 >= 0);
    if (fd1 < 0) return;

    long r = sc2(SYS_FLOCK, fd1, LOCK_EX | LOCK_NB);
    check_val("fd1 EX lock", r, 0);

    long fd2 = sc3(SYS_OPEN, (long)"/tmp/flock06", O_RDWR, 0);
    check("open fd2", fd2 >= 0);

    r = sc2(SYS_FLOCK, fd2, LOCK_EX | LOCK_NB);
    check_val("fd2 EX denied", r, -EWOULDBLOCK);

    r = sc2(SYS_FLOCK, fd1, LOCK_UN);
    check_val("fd1 unlock", r, 0);

    r = sc2(SYS_FLOCK, fd2, LOCK_EX | LOCK_NB);
    check_val("fd2 EX after unlock", r, 0);

    sc2(SYS_FLOCK, fd2, LOCK_UN);
    sc1(SYS_CLOSE, fd1);
    sc1(SYS_CLOSE, fd2);
    sc1(SYS_UNLINK, (long)"/tmp/flock06");
}

/* ── flock07: child unlock of parent's lock (via inherited fd) ── */

static void test_flock07(void) {
    puts("\n[ltp/flock07]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/flock07", O_RDWR | O_CREAT | O_TRUNC, 0666);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc2(SYS_FLOCK, fd, LOCK_EX | LOCK_NB);
    check_val("parent EX lock", r, 0);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child inherits fd — unlock via inherited fd, then lock via new fd */
        long cr = sc2(SYS_FLOCK, fd, LOCK_UN);
        if (cr != 0) sc1(SYS_EXIT, 1);
        long cfd2 = sc3(SYS_OPEN, (long)"/tmp/flock07", O_RDWR, 0);
        cr = sc2(SYS_FLOCK, cfd2, LOCK_EX | LOCK_NB);
        if (cr != 0) sc1(SYS_EXIT, 2);
        sc2(SYS_FLOCK, cfd2, LOCK_UN);
        sc1(SYS_CLOSE, cfd2);
        sc1(SYS_EXIT, 0);
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child unlock+relock", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/flock07");
}

TEST("ltp/flock01", test_flock01);
TEST("ltp/flock02", test_flock02);
TEST("ltp/flock03", test_flock03);
TEST("ltp/flock04", test_flock04);
TEST("ltp/flock06", test_flock06);
TEST("ltp/flock07", test_flock07);
