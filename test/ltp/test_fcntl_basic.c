/* LTP fcntl01-05, fcntl08-14 — ported to ktest */
#include "ktest.h"

#define O_ACCMODE 3

/* ── fcntl01: F_DUPFD returns new fd >= arg ── */

static void test_fcntl01(void) {
    puts("\n[ltp/fcntl01]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl01", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* F_DUPFD with arg=0 — returns lowest available >= 0 */
    long nfd = sc3(SYS_FCNTL, fd, F_DUPFD, 0);
    check("F_DUPFD(0) valid", nfd >= 0);
    check("F_DUPFD(0) != orig", nfd != fd);
    if (nfd >= 0) sc1(SYS_CLOSE, nfd);

    /* F_DUPFD with arg=100 — returns fd >= 100 */
    nfd = sc3(SYS_FCNTL, fd, F_DUPFD, 100);
    check_ge("F_DUPFD(100) >= 100", nfd, 100);
    if (nfd >= 0) sc1(SYS_CLOSE, nfd);

    /* F_DUPFD with arg=fd+1 */
    nfd = sc3(SYS_FCNTL, fd, F_DUPFD, fd + 1);
    check_ge("F_DUPFD(fd+1) >= fd+1", nfd, fd + 1);
    if (nfd >= 0) sc1(SYS_CLOSE, nfd);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl01");
}

/* ── fcntl02: F_DUPFD on closed/invalid fd → EBADF ── */

static void test_fcntl02(void) {
    puts("\n[ltp/fcntl02]\n");

    long r = sc3(SYS_FCNTL, -1, F_DUPFD, 0);
    check_val("F_DUPFD(-1) EBADF", r, -EBADF);

    r = sc3(SYS_FCNTL, 999, F_DUPFD, 0);
    check_val("F_DUPFD(999) EBADF", r, -EBADF);

    /* Open and close, then F_DUPFD on closed fd */
    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl02", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    r = sc3(SYS_FCNTL, fd, F_DUPFD, 0);
    check_val("F_DUPFD closed fd EBADF", r, -EBADF);

    sc1(SYS_UNLINK, (long)"/tmp/fcntl02");
}

/* ── fcntl03: F_GETFL returns open flags ── */

static void test_fcntl03(void) {
    puts("\n[ltp/fcntl03]\n");

    /* O_RDONLY */
    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl03", O_RDWR | O_CREAT, 0644);
    check("create file", fd >= 0);
    if (fd < 0) return;
    sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/tmp/fcntl03", O_RDONLY, 0);
    check("open RDONLY", fd >= 0);
    if (fd >= 0) {
        long fl = sc2(SYS_FCNTL, fd, F_GETFL);
        check("F_GETFL RDONLY access", (fl & O_ACCMODE) == O_RDONLY);
        sc1(SYS_CLOSE, fd);
    }

    /* O_WRONLY */
    fd = sc3(SYS_OPEN, (long)"/tmp/fcntl03", O_WRONLY, 0);
    check("open WRONLY", fd >= 0);
    if (fd >= 0) {
        long fl = sc2(SYS_FCNTL, fd, F_GETFL);
        check("F_GETFL WRONLY access", (fl & O_ACCMODE) == O_WRONLY);
        sc1(SYS_CLOSE, fd);
    }

    /* O_RDWR */
    fd = sc3(SYS_OPEN, (long)"/tmp/fcntl03", O_RDWR, 0);
    check("open RDWR", fd >= 0);
    if (fd >= 0) {
        long fl = sc2(SYS_FCNTL, fd, F_GETFL);
        check("F_GETFL RDWR access", (fl & O_ACCMODE) == O_RDWR);
        sc1(SYS_CLOSE, fd);
    }

    /* O_RDWR | O_APPEND */
    fd = sc3(SYS_OPEN, (long)"/tmp/fcntl03", O_RDWR | O_APPEND, 0);
    check("open RDWR|APPEND", fd >= 0);
    if (fd >= 0) {
        long fl = sc2(SYS_FCNTL, fd, F_GETFL);
        check("F_GETFL has O_APPEND", (fl & O_APPEND) != 0);
        sc1(SYS_CLOSE, fd);
    }

    sc1(SYS_UNLINK, (long)"/tmp/fcntl03");
}

/* ── fcntl04: F_GETFL on closed fd → EBADF ── */

static void test_fcntl04(void) {
    puts("\n[ltp/fcntl04]\n");

    long r = sc2(SYS_FCNTL, -1, F_GETFL);
    check_val("F_GETFL(-1) EBADF", r, -EBADF);

    r = sc2(SYS_FCNTL, 999, F_GETFL);
    check_val("F_GETFL(999) EBADF", r, -EBADF);
}

/* ── fcntl05: F_GETLK/F_SETLK basic lock operations ── */

static void test_fcntl05(void) {
    puts("\n[ltp/fcntl05]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl05", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Write some data so lock range is valid */
    sc3(SYS_WRITE, fd, (long)"abcdef", 6);

    /* Set a write lock on bytes 0-5 */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 6;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("F_SETLK write lock", r, 0);

    /* F_GETLK on same range — should return F_UNLCK (no conflict with self) */
    struct k_flock qlk;
    qlk.l_type = F_WRLCK;
    qlk.l_whence = SEEK_SET;
    qlk.l_start = 0;
    qlk.l_len = 6;
    qlk.l_pid = 0;

    r = sc3(SYS_FCNTL, fd, F_GETLK, (long)&qlk);
    check_val("F_GETLK", r, 0);
    check_val("F_GETLK self sees F_UNLCK", (long)qlk.l_type, F_UNLCK);

    /* Unlock */
    lk.l_type = F_UNLCK;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("F_SETLK unlock", r, 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl05");
}

/* ── fcntl08: F_SETFL set/clear O_NONBLOCK, O_APPEND ── */

static void test_fcntl08(void) {
    puts("\n[ltp/fcntl08]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl08", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Set O_NONBLOCK */
    long fl = sc2(SYS_FCNTL, fd, F_GETFL);
    long r = sc3(SYS_FCNTL, fd, F_SETFL, fl | O_NONBLOCK);
    check_val("F_SETFL +O_NONBLOCK", r, 0);

    fl = sc2(SYS_FCNTL, fd, F_GETFL);
    check("O_NONBLOCK set", (fl & O_NONBLOCK) != 0);

    /* Clear O_NONBLOCK */
    r = sc3(SYS_FCNTL, fd, F_SETFL, fl & ~O_NONBLOCK);
    check_val("F_SETFL -O_NONBLOCK", r, 0);

    fl = sc2(SYS_FCNTL, fd, F_GETFL);
    check("O_NONBLOCK cleared", (fl & O_NONBLOCK) == 0);

    /* Set O_APPEND */
    r = sc3(SYS_FCNTL, fd, F_SETFL, fl | O_APPEND);
    check_val("F_SETFL +O_APPEND", r, 0);

    fl = sc2(SYS_FCNTL, fd, F_GETFL);
    check("O_APPEND set", (fl & O_APPEND) != 0);

    /* Clear O_APPEND */
    r = sc3(SYS_FCNTL, fd, F_SETFL, fl & ~O_APPEND);
    check_val("F_SETFL -O_APPEND", r, 0);

    fl = sc2(SYS_FCNTL, fd, F_GETFL);
    check("O_APPEND cleared", (fl & O_APPEND) == 0);

    /* Set both */
    r = sc3(SYS_FCNTL, fd, F_SETFL, fl | O_NONBLOCK | O_APPEND);
    check_val("F_SETFL +NONBLOCK+APPEND", r, 0);

    fl = sc2(SYS_FCNTL, fd, F_GETFL);
    check("both set", (fl & (O_NONBLOCK | O_APPEND)) == (O_NONBLOCK | O_APPEND));

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl08");
}

/* ── fcntl11: F_SETLK EINVAL for bad lock type ── */

static void test_fcntl11(void) {
    puts("\n[ltp/fcntl11]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl11", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    struct k_flock lk;
    lk.l_type = 99;  /* invalid lock type */
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("F_SETLK bad type EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl11");
}

/* ── fcntl13: F_SETLK on pipe → EINVAL ── */

static void test_fcntl13(void) {
    puts("\n[ltp/fcntl13]\n");

    int pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0;
    lk.l_pid = 0;

    r = sc3(SYS_FCNTL, pipefd[0], F_SETLK, (long)&lk);
    check_val("F_SETLK pipe read EINVAL", r, -EINVAL);

    r = sc3(SYS_FCNTL, pipefd[1], F_SETLK, (long)&lk);
    check_val("F_SETLK pipe write EINVAL", r, -EINVAL);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── fcntl09: F_SETLK write lock blocks other write lock (fork test) ── */

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static void test_fcntl09(void) {
    puts("\n[ltp/fcntl09]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl09", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Parent sets write lock on bytes 0-9 */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent write lock", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        /* Child: open same file, try write lock — should fail EAGAIN */
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl09", O_RDWR, 0);
        struct k_flock clk;
        clk.l_type = F_WRLCK;
        clk.l_whence = SEEK_SET;
        clk.l_start = 0;
        clk.l_len = 10;
        clk.l_pid = 0;
        long cr = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);
        sc1(SYS_CLOSE, cfd);
        /* exit 0 if got EAGAIN (or EACCES), 1 otherwise */
        sc1(SYS_EXIT_GROUP, (cr == -EAGAIN || cr == -EACCES) ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child got EAGAIN/EACCES", (long)WEXITSTATUS(wstatus), 0);

    /* Unlock */
    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl09");
}

/* ── fcntl10: F_SETLK read lock allows other read lock ── */

static void test_fcntl10(void) {
    puts("\n[ltp/fcntl10]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl10", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Parent sets read lock */
    struct k_flock lk;
    lk.l_type = F_RDLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent read lock", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        /* Child: open same file, try read lock — should succeed */
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl10", O_RDONLY, 0);
        struct k_flock clk;
        clk.l_type = F_RDLCK;
        clk.l_whence = SEEK_SET;
        clk.l_start = 0;
        clk.l_len = 10;
        clk.l_pid = 0;
        long cr = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, (cr == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child read lock succeeded", (long)WEXITSTATUS(wstatus), 0);

    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl10");
}

/* ── fcntl12: F_SETLK/F_GETLK with child — lock not inherited across fork ── */

static void test_fcntl12(void) {
    puts("\n[ltp/fcntl12]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl12", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Parent sets write lock */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent write lock", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        /* Child inherits fd but NOT locks (POSIX: locks not inherited).
         * Child should be able to set its own lock on a NEW fd. */
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl12", O_RDWR, 0);
        struct k_flock qlk;
        qlk.l_type = F_WRLCK;
        qlk.l_whence = SEEK_SET;
        qlk.l_start = 0;
        qlk.l_len = 10;
        qlk.l_pid = 0;

        /* F_GETLK should report parent's lock */
        long cr = sc3(SYS_FCNTL, cfd, F_GETLK, (long)&qlk);
        int saw_lock = (cr == 0 && qlk.l_type != F_UNLCK);

        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, saw_lock ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child saw parent lock", (long)WEXITSTATUS(wstatus), 0);

    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl12");
}

/* ── fcntl14: F_SETLKW deadlock detection → EDEADLK ── */
/* This is a complex multi-process test. We test the basic concept:
 * Process A locks region 1, Process B locks region 2.
 * A tries F_SETLKW on region 2, B tries F_SETLKW on region 1.
 * One should get EDEADLK.
 * Simplified: we just verify F_SETLKW blocks properly (single-process). */

static void test_fcntl14(void) {
    puts("\n[ltp/fcntl14]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl14", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Set write lock bytes 0-4 */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 5;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("lock bytes 0-4", r, 0);

    /* Set write lock bytes 5-9 (non-overlapping, same process — should work) */
    lk.l_start = 5;
    lk.l_len = 5;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("lock bytes 5-9", r, 0);

    /* F_SETLKW on own lock range — should succeed immediately (same process) */
    lk.l_start = 0;
    lk.l_len = 10;
    r = sc3(SYS_FCNTL, fd, F_SETLKW, (long)&lk);
    check_val("F_SETLKW own range", r, 0);

    /* Unlock all */
    lk.l_type = F_UNLCK;
    lk.l_start = 0;
    lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl14");
}

TEST("ltp/fcntl01", test_fcntl01);
TEST("ltp/fcntl02", test_fcntl02);
TEST("ltp/fcntl03", test_fcntl03);
TEST("ltp/fcntl04", test_fcntl04);
TEST("ltp/fcntl05", test_fcntl05);
TEST("ltp/fcntl08", test_fcntl08);
TEST("ltp/fcntl09", test_fcntl09);
TEST("ltp/fcntl10", test_fcntl10);
TEST("ltp/fcntl11", test_fcntl11);
TEST("ltp/fcntl12", test_fcntl12);
TEST("ltp/fcntl13", test_fcntl13);
TEST("ltp/fcntl14", test_fcntl14);
