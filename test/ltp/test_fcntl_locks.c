/* LTP fcntl15-33 — advanced lock tests, ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── fcntl15: F_SETLKW waiting for lock release ── */
/* Parent locks, child tries F_SETLKW, parent unlocks, child gets lock. */

static void test_fcntl15(void) {
    puts("\n[ltp/fcntl15]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl15", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Parent: write lock bytes 0-9 */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent lock", r, 0);

    /* Pipe for synchronization */
    int pipefd[2];
    r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        sc1(SYS_CLOSE, pipefd[0]);
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl15", O_RDWR, 0);

        /* Signal parent we're about to block */
        sc3(SYS_WRITE, pipefd[1], (long)"R", 1);
        sc1(SYS_CLOSE, pipefd[1]);

        /* F_SETLKW — should block until parent unlocks */
        struct k_flock clk;
        clk.l_type = F_WRLCK;
        clk.l_whence = SEEK_SET;
        clk.l_start = 0;
        clk.l_len = 10;
        clk.l_pid = 0;
        long cr = sc3(SYS_FCNTL, cfd, F_SETLKW, (long)&clk);

        /* Unlock and exit */
        clk.l_type = F_UNLCK;
        sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, (cr == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    sc1(SYS_CLOSE, pipefd[1]);

    /* Wait for child to be ready */
    char buf[1];
    sc3(SYS_READ, pipefd[0], (long)buf, 1);
    sc1(SYS_CLOSE, pipefd[0]);

    /* Small delay then unlock — child should unblock */
    /* Use nanosleep for 50ms */
    struct { long sec; long nsec; } ts = { 0, 50000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child got lock after wait", (long)WEXITSTATUS(wstatus), 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl15");
}

/* ── fcntl17: F_SETLK upgrade read→write lock ── */

static void test_fcntl17(void) {
    puts("\n[ltp/fcntl17]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl17", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Set read lock */
    struct k_flock lk;
    lk.l_type = F_RDLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("read lock", r, 0);

    /* Upgrade to write lock (same process) */
    lk.l_type = F_WRLCK;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("upgrade to write lock", r, 0);

    /* Downgrade back to read lock */
    lk.l_type = F_RDLCK;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("downgrade to read lock", r, 0);

    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl17");
}

/* ── fcntl19: F_SETLK unlock specific byte range ── */

static void test_fcntl19(void) {
    puts("\n[ltp/fcntl19]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl19", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"0123456789", 10);

    /* Lock entire file */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("lock all", r, 0);

    /* Unlock bytes 3-6 (middle) — splits lock into two regions */
    lk.l_type = F_UNLCK;
    lk.l_start = 3;
    lk.l_len = 4;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("unlock middle", r, 0);

    /* Child should be able to lock bytes 3-6 but not 0-2 */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl19", O_RDWR, 0);
        struct k_flock clk;

        /* Should succeed: bytes 3-6 are unlocked */
        clk.l_type = F_WRLCK;
        clk.l_whence = SEEK_SET;
        clk.l_start = 3;
        clk.l_len = 4;
        clk.l_pid = 0;
        long r1 = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);

        /* Should fail: bytes 0-2 still locked */
        clk.l_start = 0;
        clk.l_len = 3;
        long r2 = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);

        sc1(SYS_CLOSE, cfd);
        int ok = (r1 == 0) && (r2 == -EAGAIN || r2 == -EACCES);
        sc1(SYS_EXIT_GROUP, ok ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("partial unlock correct", (long)WEXITSTATUS(wstatus), 0);

    /* Cleanup */
    lk.l_type = F_UNLCK;
    lk.l_start = 0;
    lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl19");
}

/* ── fcntl21: F_GETLK returns conflicting lock info ── */

static void test_fcntl21(void) {
    puts("\n[ltp/fcntl21]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl21", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Parent: write lock bytes 2-7 */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 2;
    lk.l_len = 6;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent lock 2-7", r, 0);

    long ppid = sc0(SYS_GETPID);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl21", O_RDWR, 0);
        struct k_flock qlk;
        qlk.l_type = F_WRLCK;
        qlk.l_whence = SEEK_SET;
        qlk.l_start = 0;
        qlk.l_len = 10;
        qlk.l_pid = 0;

        long cr = sc3(SYS_FCNTL, cfd, F_GETLK, (long)&qlk);
        int ok = (cr == 0) &&
                 (qlk.l_type == F_WRLCK) &&
                 (qlk.l_pid == (int)ppid) &&
                 (qlk.l_start >= 0);  /* lock info returned */

        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, ok ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child saw lock info", (long)WEXITSTATUS(wstatus), 0);

    lk.l_type = F_UNLCK;
    lk.l_start = 0;
    lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl21");
}

/* ── fcntl23: F_SETLK with multiple processes ── */

static void test_fcntl23(void) {
    puts("\n[ltp/fcntl23]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl23", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"0123456789ABCDEF", 16);

    /* Parent locks bytes 0-7 */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 8;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent lock 0-7", r, 0);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl23", O_RDWR, 0);
        struct k_flock clk;

        /* Child locks bytes 8-15 (non-overlapping — should succeed) */
        clk.l_type = F_WRLCK;
        clk.l_whence = SEEK_SET;
        clk.l_start = 8;
        clk.l_len = 8;
        clk.l_pid = 0;
        long r1 = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);

        /* Child tries overlapping lock — should fail */
        clk.l_start = 4;
        clk.l_len = 8;
        long r2 = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&clk);

        sc1(SYS_CLOSE, cfd);
        int ok = (r1 == 0) && (r2 == -EAGAIN || r2 == -EACCES);
        sc1(SYS_EXIT_GROUP, ok ? 0 : 1);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("multi-process locks correct", (long)WEXITSTATUS(wstatus), 0);

    lk.l_type = F_UNLCK;
    lk.l_start = 0;
    lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl23");
}

/* ── fcntl29: F_DUPFD_CLOEXEC ── */

static void test_fcntl29(void) {
    puts("\n[ltp/fcntl29]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl29", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* F_DUPFD_CLOEXEC with arg=0 */
    long nfd = sc3(SYS_FCNTL, fd, F_DUPFD_CLOEXEC, 0);
    check("F_DUPFD_CLOEXEC valid", nfd >= 0);
    if (nfd >= 0) {
        long flags = sc2(SYS_FCNTL, nfd, F_GETFD);
        check("FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);
        sc1(SYS_CLOSE, nfd);
    }

    /* F_DUPFD_CLOEXEC with arg=50 */
    nfd = sc3(SYS_FCNTL, fd, F_DUPFD_CLOEXEC, 50);
    check_ge("F_DUPFD_CLOEXEC(50) >= 50", nfd, 50);
    if (nfd >= 0) {
        long flags = sc2(SYS_FCNTL, nfd, F_GETFD);
        check("FD_CLOEXEC set (50)", (flags & FD_CLOEXEC) != 0);
        sc1(SYS_CLOSE, nfd);
    }

    /* Verify original fd does NOT have FD_CLOEXEC */
    long orig_flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("orig no FD_CLOEXEC", (orig_flags & FD_CLOEXEC) == 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl29");
}

/* ── fcntl30: F_SETFD set/get FD_CLOEXEC ── */

static void test_fcntl30(void) {
    puts("\n[ltp/fcntl30]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl30", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Initially no FD_CLOEXEC */
    long flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("no initial CLOEXEC", (flags & FD_CLOEXEC) == 0);

    /* Set FD_CLOEXEC */
    long r = sc3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    check_val("F_SETFD CLOEXEC", r, 0);

    flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("FD_CLOEXEC set", (flags & FD_CLOEXEC) != 0);

    /* Clear FD_CLOEXEC */
    r = sc3(SYS_FCNTL, fd, F_SETFD, 0);
    check_val("F_SETFD clear", r, 0);

    flags = sc2(SYS_FCNTL, fd, F_GETFD);
    check("FD_CLOEXEC cleared", (flags & FD_CLOEXEC) == 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl30");
}

/* ── fcntl33: lock on closed fd is released ── */

static void test_fcntl33(void) {
    puts("\n[ltp/fcntl33]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl33", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    /* Set write lock */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("set lock", r, 0);

    /* Close fd — lock should be released */
    sc1(SYS_CLOSE, fd);

    /* Child should be able to lock the file now */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl33", O_RDWR, 0);
        struct k_flock clk;
        clk.l_type = F_WRLCK;
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
    check_val("lock released on close", (long)WEXITSTATUS(wstatus), 0);

    sc1(SYS_UNLINK, (long)"/tmp/fcntl33");
}

TEST("ltp/fcntl15", test_fcntl15);
TEST("ltp/fcntl17", test_fcntl17);
TEST("ltp/fcntl19", test_fcntl19);
TEST("ltp/fcntl21", test_fcntl21);
TEST("ltp/fcntl23", test_fcntl23);
TEST("ltp/fcntl29", test_fcntl29);
TEST("ltp/fcntl30", test_fcntl30);
TEST("ltp/fcntl33", test_fcntl33);
