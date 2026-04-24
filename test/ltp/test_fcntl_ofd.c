/* fcntl OFD/F_SETLEASE/F_SETOWN_EX/F_*PIPE_SZ tests — covers LTP fcntl34-37 */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── fcntl13: bad-userptr must return -EFAULT, never page-fault ── */

static void test_fcntl13_bad_ptr(void) {
    puts("\n[ltp/fcntl13_bad_ptr]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl13p", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_FCNTL, fd, F_SETLK, 0);
    check_val("F_SETLK NULL ptr EFAULT", r, -EFAULT);

    r = sc3(SYS_FCNTL, fd, F_GETLK, 0);
    check_val("F_GETLK NULL ptr EFAULT", r, -EFAULT);

    r = sc3(SYS_FCNTL, fd, F_SETLK, 0x700000000000L);
    check_val("F_SETLK unmapped ptr EFAULT", r, -EFAULT);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl13p");
}

/* ── F_GETLK preserves query range when no conflict ── */

static void test_fcntl_getlk_unlck_keeps_range(void) {
    puts("\n[ltp/fcntl_getlk_unlck]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_gq", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"abcdefghij", 10);

    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 42;
    lk.l_len = 7;
    lk.l_pid = 12345;  /* must stay unchanged on no-conflict */

    long r = sc3(SYS_FCNTL, fd, F_GETLK, (long)&lk);
    check_val("F_GETLK no conflict rc", r, 0);
    check_val("l_type == F_UNLCK", (long)lk.l_type, F_UNLCK);
    check_val("l_start preserved", (long)lk.l_start, 42);
    check_val("l_len preserved", (long)lk.l_len, 7);
    check_val("l_pid preserved (Linux fs/locks.c)", (long)lk.l_pid, 12345);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_gq");
}

/* ── Range split: lock [0,9], unlock middle [3,6] => two locks ── */

static void test_fcntl_range_split(void) {
    puts("\n[ltp/fcntl_range_split]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_split", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"0123456789", 10);

    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 10;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("lock 0-9", r, 0);

    lk.l_type = F_UNLCK;
    lk.l_start = 3;
    lk.l_len = 4;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("unlock middle 3-6", r, 0);

    /* Verify via child that [0,2] still locked and [3,6] free */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }
    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl_split", O_RDWR, 0);
        struct k_flock q;
        q.l_type = F_WRLCK; q.l_whence = SEEK_SET;
        q.l_start = 3; q.l_len = 4; q.l_pid = 0;
        long r1 = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&q);
        q.l_start = 0; q.l_len = 3;
        long r2 = sc3(SYS_FCNTL, cfd, F_SETLK, (long)&q);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP,
            (r1 == 0 && (r2 == -EAGAIN || r2 == -EACCES)) ? 0 : 1);
        __builtin_unreachable();
    }
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("split verified", (long)WEXITSTATUS(ws), 0);

    lk.l_type = F_UNLCK; lk.l_start = 0; lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_split");
}

/* ── F_OFD_SETLK: owned by open-file-description, not pid ── */

static void test_fcntl_ofd_basic(void) {
    puts("\n[ltp/fcntl_ofd_basic]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_ofd", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"0123456789", 10);

    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 5;
    lk.l_pid = 0;

    long r = sc3(SYS_FCNTL, fd, F_OFD_SETLK, (long)&lk);
    check_val("F_OFD_SETLK write 0-4", r, 0);

    /* Second open on same process — OFD locks conflict across open fds */
    long fd2 = sc3(SYS_OPEN, (long)"/tmp/fcntl_ofd", O_RDWR, 0);
    check("open again", fd2 >= 0);
    lk.l_type = F_WRLCK;
    r = sc3(SYS_FCNTL, fd2, F_OFD_SETLK, (long)&lk);
    check_val("OFD conflict same proc", r, -EAGAIN);

    /* F_OFD_GETLK should report -1 pid (OFD sentinel) */
    struct k_flock q;
    q.l_type = F_WRLCK; q.l_whence = SEEK_SET;
    q.l_start = 0; q.l_len = 5; q.l_pid = 0;
    r = sc3(SYS_FCNTL, fd2, F_OFD_GETLK, (long)&q);
    check_val("OFD GETLK rc", r, 0);
    check_val("OFD GETLK l_type=F_WRLCK", (long)q.l_type, F_WRLCK);
    check_val("OFD GETLK l_pid=-1", (long)q.l_pid, -1);

    /* Non-zero l_pid on F_OFD_SETLK input -> EINVAL */
    lk.l_pid = 1;
    r = sc3(SYS_FCNTL, fd2, F_OFD_SETLK, (long)&lk);
    check_val("OFD SETLK l_pid!=0 EINVAL", r, -EINVAL);

    /* Unlock via first fd */
    lk.l_type = F_UNLCK; lk.l_pid = 0;
    r = sc3(SYS_FCNTL, fd, F_OFD_SETLK, (long)&lk);
    check_val("OFD unlock", r, 0);

    /* Second open can now acquire */
    lk.l_type = F_WRLCK;
    r = sc3(SYS_FCNTL, fd2, F_OFD_SETLK, (long)&lk);
    check_val("OFD lock after unlock", r, 0);

    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd2, F_OFD_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_CLOSE, fd2);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_ofd");
}

/* ── F_SETLEASE / F_GETLEASE — fcntl23 style ── */

static void test_fcntl_setlease(void) {
    puts("\n[ltp/fcntl_setlease]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_lease", O_RDONLY | O_CREAT, 0644);
    check("open RDONLY", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_FCNTL, fd, F_SETLEASE, F_RDLCK);
    check_val("F_SETLEASE F_RDLCK", r, 0);

    r = sc2(SYS_FCNTL, fd, F_GETLEASE);
    check_val("F_GETLEASE == F_RDLCK", r, F_RDLCK);

    r = sc3(SYS_FCNTL, fd, F_SETLEASE, F_UNLCK);
    check_val("F_SETLEASE F_UNLCK", r, 0);

    r = sc2(SYS_FCNTL, fd, F_GETLEASE);
    check_val("F_GETLEASE == F_UNLCK after remove", r, F_UNLCK);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_lease");
}

/* ── F_SETLEASE F_WRLCK requires sole fd-holder ── */

static void test_fcntl_setlease_wr_conflict(void) {
    puts("\n[ltp/fcntl_setlease_wr]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_lease2", O_RDONLY | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Only one fd — F_WRLCK must succeed */
    long r = sc3(SYS_FCNTL, fd, F_SETLEASE, F_WRLCK);
    check_val("F_SETLEASE F_WRLCK alone", r, 0);

    /* F_SETLEASE F_RDLCK on O_RDWR must fail EAGAIN */
    long fd2 = sc3(SYS_OPEN, (long)"/tmp/fcntl_lease2", O_RDWR, 0);
    check("open RDWR", fd2 >= 0);
    r = sc3(SYS_FCNTL, fd2, F_SETLEASE, F_RDLCK);
    check_val("F_SETLEASE RD on RDWR EAGAIN", r, -EAGAIN);

    sc3(SYS_FCNTL, fd, F_SETLEASE, F_UNLCK);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_CLOSE, fd2);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_lease2");
}

/* ── F_GETOWN_EX / F_SETOWN_EX ── */

static void test_fcntl_owner_ex(void) {
    puts("\n[ltp/fcntl_owner_ex]\n");

    int pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    struct k_f_owner_ex ex = { F_OWNER_PID, 0 };
    r = sc3(SYS_FCNTL, pipefd[0], F_SETOWN_EX, (long)&ex);
    check_val("F_SETOWN_EX PID", r, 0);

    ex.type = F_OWNER_TID;
    r = sc3(SYS_FCNTL, pipefd[0], F_SETOWN_EX, (long)&ex);
    check_val("F_SETOWN_EX TID", r, 0);

    ex.type = 99;
    r = sc3(SYS_FCNTL, pipefd[0], F_SETOWN_EX, (long)&ex);
    check_val("F_SETOWN_EX bad type EINVAL", r, -EINVAL);

    r = sc3(SYS_FCNTL, pipefd[0], F_GETOWN_EX, (long)&ex);
    check_val("F_GETOWN_EX rc", r, 0);

    r = sc3(SYS_FCNTL, pipefd[0], F_SETOWN_EX, 0);
    check_val("F_SETOWN_EX NULL EFAULT", r, -EFAULT);

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── F_SETPIPE_SZ error cases (fcntl37 style) ── */

#define SYS_SETUID 105

static void test_fcntl_pipe_sz(void) {
    puts("\n[ltp/fcntl_pipe_sz]\n");

    int pipefd[2];
    long r = sc1(SYS_PIPE, (long)pipefd);
    check_val("pipe", r, 0);
    if (r != 0) return;

    /* F_GETPIPE_SZ returns buffer size (positive) */
    r = sc2(SYS_FCNTL, pipefd[1], F_GETPIPE_SZ);
    check("F_GETPIPE_SZ positive", r > 0);

    /* Beyond 1<<31 => EINVAL */
    r = sc3(SYS_FCNTL, pipefd[1], F_SETPIPE_SZ, (1UL << 31) + 1);
    check_val("SETPIPE_SZ >=2^31 EINVAL", r, -EINVAL);

    /* Linux: Root (CAP_SYS_RESOURCE) darf ueber pipe-max-size. Der
     * EPERM-Fall wird im forked Child mit setuid(nobody) getestet. */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_SETUID, 65534);
        long r2 = sc3(SYS_FCNTL, pipefd[1], F_SETPIPE_SZ, 1024 * 1024 * 4);
        sc1(SYS_EXIT, r2 == -EPERM ? 0 : 1);
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("SETPIPE_SZ > max EPERM (unpriv)", WEXITSTATUS(status), 0);

    /* Root self darf es: 2 MiB setzen (pipe-max-size default 1 MiB; Linux:
     * F_SETPIPE_SZ ist nur gegen pipe-max-size gecappt fuer unprivileged.
     * 2 MiB = Buddy-MaxOrder, 4 MiB waere zu gross fuer contiguous alloc). */
    r = sc3(SYS_FCNTL, pipefd[1], F_SETPIPE_SZ, 1024 * 1024 * 2);
    check("SETPIPE_SZ > max as root OK", r > 0);

    /* F_GETPIPE_SZ on non-pipe => EINVAL */
    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_p", O_RDWR | O_CREAT, 0644);
    r = sc2(SYS_FCNTL, fd, F_GETPIPE_SZ);
    check_val("F_GETPIPE_SZ non-pipe EINVAL", r, -EINVAL);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_p");

    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── flock(2) and fcntl coexistence (different lock tables, no cross conflict) ── */

static void test_flock_vs_fcntl(void) {
    puts("\n[ltp/flock_vs_fcntl]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/flockcofl", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc2(SYS_FLOCK, fd, LOCK_EX);
    check_val("flock LOCK_EX", r, 0);

    struct k_flock lk;
    lk.l_type = F_WRLCK; lk.l_whence = SEEK_SET;
    lk.l_start = 0; lk.l_len = 0; lk.l_pid = 0;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("fcntl on flock'd file", r, 0);

    lk.l_type = F_UNLCK;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc2(SYS_FLOCK, fd, LOCK_UN);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/flockcofl");
}

/* ── F_SETLK with SEEK_CUR / SEEK_END ── */

static void test_fcntl_seek_cur_end(void) {
    puts("\n[ltp/fcntl_seek_cur]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_sk", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"0123456789", 10);
    sc3(SYS_LSEEK, fd, 3, SEEK_SET);  /* offset=3 */

    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_CUR;
    lk.l_start = 2;    /* effective start=5 */
    lk.l_len = 2;      /* [5,6] */
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("SEEK_CUR lock", r, 0);

    /* GETLK with SEEK_CUR at same place should see UNLCK (same owner) */
    lk.l_type = F_WRLCK;
    lk.l_start = 2; lk.l_len = 2;
    r = sc3(SYS_FCNTL, fd, F_GETLK, (long)&lk);
    check_val("GETLK SEEK_CUR rc", r, 0);
    check_val("GETLK SEEK_CUR self=UNLCK", (long)lk.l_type, F_UNLCK);

    /* SEEK_END with negative start (lock last 3 bytes [7,9]) */
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_END;
    lk.l_start = -3;
    lk.l_len = 3;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("SEEK_END lock", r, 0);

    /* l_whence bad value */
    lk.l_whence = 99;
    lk.l_start = 0; lk.l_len = 0;
    r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("bad l_whence EINVAL", r, -EINVAL);

    lk.l_whence = SEEK_SET; lk.l_type = F_UNLCK;
    lk.l_start = 0; lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_sk");
}

/* ── F_SETLK bad ptr + F_GETLK bad ptr coverage ── */

static void test_fcntl_bad_ptrs(void) {
    puts("\n[ltp/fcntl_bad_ptrs]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_bp", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_FCNTL, fd, F_OFD_SETLK, 0);
    check_val("OFD_SETLK NULL EFAULT", r, -EFAULT);

    r = sc3(SYS_FCNTL, fd, F_OFD_GETLK, 0);
    check_val("OFD_GETLK NULL EFAULT", r, -EFAULT);

    r = sc3(SYS_FCNTL, fd, F_GETOWN_EX, 0);
    check_val("GETOWN_EX NULL EFAULT", r, -EFAULT);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_bp");
}

/* ── F_GETLK reports conflict with correct range/pid (fork'd child) ── */

static void test_fcntl_getlk_reports_range(void) {
    puts("\n[ltp/fcntl_getlk_range]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_gr", O_RDWR | O_CREAT, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"0123456789", 10);

    /* Parent: F_WRLCK bytes 2-7 */
    struct k_flock lk;
    lk.l_type = F_WRLCK; lk.l_whence = SEEK_SET;
    lk.l_start = 2; lk.l_len = 6; lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    check_val("parent lock 2-7", r, 0);

    long parent_pid = sc0(SYS_GETPID);
    long pid = sc0(SYS_FORK);
    if (pid < 0) { sc1(SYS_CLOSE, fd); return; }

    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl_gr", O_RDWR, 0);
        struct k_flock q;
        q.l_type = F_WRLCK; q.l_whence = SEEK_SET;
        q.l_start = 0; q.l_len = 10; q.l_pid = 0;
        long cr = sc3(SYS_FCNTL, cfd, F_GETLK, (long)&q);
        int ok = (cr == 0) && (q.l_type == F_WRLCK) &&
                 (q.l_start == 2) && (q.l_len == 6) &&
                 (q.l_pid == (int)parent_pid);
        sc1(SYS_CLOSE, cfd);
        sc1(SYS_EXIT_GROUP, ok ? 0 : 1);
        __builtin_unreachable();
    }
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("child saw lock details", (long)WEXITSTATUS(ws), 0);

    lk.l_type = F_UNLCK; lk.l_start = 0; lk.l_len = 0;
    sc3(SYS_FCNTL, fd, F_SETLK, (long)&lk);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_gr");
}

/* ── fcntl13 testcase 3: bad l_whence on non-seekable fd returns EINVAL ── */

static void test_fcntl_bad_whence_order(void) {
    puts("\n[ltp/fcntl_bad_whence_order]\n");

    /* stderr is non-FD_FILE (FD_SERIAL). flock.l_whence=-1 must give EINVAL,
     * not EBADF or success. */
    struct k_flock lk;
    lk.l_type = F_WRLCK;
    lk.l_whence = -1;
    lk.l_start = 0;
    lk.l_len = 0;
    lk.l_pid = 0;
    long r = sc3(SYS_FCNTL, 2, F_SETLK, (long)&lk);
    check_val("F_SETLK bad whence on serial EINVAL", r, -EINVAL);
}

/* ── F_SETLEASE on pipe must fail ── */

static void test_fcntl_setlease_pipe(void) {
    puts("\n[ltp/fcntl_setlease_pipe]\n");

    int pipefd[2];
    sc1(SYS_PIPE, (long)pipefd);
    long r = sc3(SYS_FCNTL, pipefd[0], F_SETLEASE, F_RDLCK);
    check_val("SETLEASE on pipe EINVAL", r, -EINVAL);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

/* ── F_NOTIFY accepted (no-op) ── */

static void test_fcntl_notify_noop(void) {
    puts("\n[ltp/fcntl_notify]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp", O_RDONLY | O_DIRECTORY, 0);
    check("open dir", fd >= 0);
    if (fd < 0) return;

    long r = sc3(SYS_FCNTL, fd, F_NOTIFY, DN_CREATE | DN_MULTISHOT);
    check_val("F_NOTIFY rc=0", r, 0);

    sc1(SYS_CLOSE, fd);
}

/* ── DN_ATTRIB: parent + subdir watch beide queued sich ein ──────────
 * LTP fcntl38-Regression: bei chmod auf Kind sollen sowohl Self-Watch
 * auf dem Kind als auch Parent-Watch auf dem Verzeichnis feuern. Ueber
 * SYS_SIGTIMEDWAIT werden beide pending Eintraege konsekutiv geerntet. */
#define T_SIGRTMIN_1 35   /* SIGRTMIN(34) + 1 */

struct k_siginfo_32 {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  _pad0;
    uint64_t _pad[16];
};

static void test_dnotify_parent_plus_subdir(void) {
    puts("\n[ltp/fcntl38_dnotify_parent_subdir]\n");

    sc3(SYS_MKDIR, (long)"/tmp/dn38_parent", 0700, 0);
    sc3(SYS_MKDIR, (long)"/tmp/dn38_parent/kid", 0700, 0);

    long parent_fd = sc3(SYS_OPEN, (long)"/tmp/dn38_parent",
                         O_RDONLY | O_DIRECTORY, 0);
    long subdir_fd = sc3(SYS_OPEN, (long)"/tmp/dn38_parent/kid",
                         O_RDONLY | O_DIRECTORY, 0);
    check("open parent dir", parent_fd >= 0);
    check("open subdir", subdir_fd >= 0);
    if (parent_fd < 0 || subdir_fd < 0) return;

    /* Block SIGRTMIN+1 so sigtimedwait konsumiert die Pendings gezielt */
    uint64_t mask = 1ULL << (T_SIGRTMIN_1 - 1);
    sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&mask, 0, 8);

    sc3(SYS_FCNTL, parent_fd, F_SETSIG, T_SIGRTMIN_1);
    sc3(SYS_FCNTL, subdir_fd, F_SETSIG, T_SIGRTMIN_1);
    sc3(SYS_FCNTL, parent_fd, F_NOTIFY, DN_ATTRIB | DN_MULTISHOT);
    sc3(SYS_FCNTL, subdir_fd, F_NOTIFY, DN_ATTRIB | DN_MULTISHOT);

    sc3(SYS_CHMOD, (long)"/tmp/dn38_parent/kid", 0755, 0);

    /* Beide fds müssen signalisiert worden sein — ernte über sigtimedwait */
    int saw_parent = 0, saw_subdir = 0;
    for (int i = 0; i < 4; i++) {
        struct k_siginfo_32 info = {0};
        struct k_timespec ts = { 0, 100000000 }; /* 100 ms */
        long r = sc4(SYS_RT_SIGTIMEDWAIT, (long)&mask, (long)&info, (long)&ts, 8);
        if (r != T_SIGRTMIN_1) break;
        int fd = *(int *)((char *)&info + 24);
        if (fd == (int)parent_fd) saw_parent = 1;
        if (fd == (int)subdir_fd) saw_subdir = 1;
        if (saw_parent && saw_subdir) break;
    }

    check("parent watch fired", saw_parent);
    check("subdir watch fired", saw_subdir);

    sc3(SYS_FCNTL, parent_fd, F_NOTIFY, 0);
    sc3(SYS_FCNTL, subdir_fd, F_NOTIFY, 0);
    sc1(SYS_CLOSE, parent_fd);
    sc1(SYS_CLOSE, subdir_fd);
    sc1(SYS_RMDIR, (long)"/tmp/dn38_parent/kid");
    sc1(SYS_RMDIR, (long)"/tmp/dn38_parent");
}

TEST("ltp/fcntl13_bad_ptr",            test_fcntl13_bad_ptr);
TEST("ltp/fcntl_getlk_unlck_range",    test_fcntl_getlk_unlck_keeps_range);
TEST("ltp/fcntl_range_split",          test_fcntl_range_split);
TEST("ltp/fcntl_ofd_basic",            test_fcntl_ofd_basic);
TEST("ltp/fcntl_setlease",             test_fcntl_setlease);
TEST("ltp/fcntl_setlease_wr",          test_fcntl_setlease_wr_conflict);
TEST("ltp/fcntl_owner_ex",             test_fcntl_owner_ex);
TEST("ltp/fcntl_pipe_sz",              test_fcntl_pipe_sz);
TEST("ltp/flock_vs_fcntl",             test_flock_vs_fcntl);
TEST("ltp/fcntl_seek_cur_end",         test_fcntl_seek_cur_end);
TEST("ltp/fcntl_bad_ptrs",             test_fcntl_bad_ptrs);
TEST("ltp/fcntl_getlk_range",          test_fcntl_getlk_reports_range);
TEST("ltp/fcntl_setlease_pipe",        test_fcntl_setlease_pipe);
TEST("ltp/fcntl_notify",               test_fcntl_notify_noop);
TEST("ltp/fcntl38_dnotify_both",       test_dnotify_parent_plus_subdir);

/* F_SETOWN_EX mit F_OWNER_TID: SIGIO-Zustellung an den Thread, nicht an
 * proc_find(tid) (tid != pid im allgemeinen Fall). */
static void test_fcntl_owner_tid_signal(void) {
    puts("\n[ltp/fcntl_owner_tid_signal]\n");

    int fds[2];
    sc1(SYS_PIPE, (long)fds);

    /* O_ASYNC + F_SETSIG(SIGUSR1) + F_SETOWN_EX(TID, my_tid) */
    long cur_flags = sc3(SYS_FCNTL, fds[0], F_GETFL, 0);
    sc3(SYS_FCNTL, fds[0], F_SETFL, cur_flags | O_ASYNC);
    sc3(SYS_FCNTL, fds[0], F_SETSIG, SIGUSR1);

    long mytid = sc0(SYS_GETTID);
    struct k_f_owner_ex oex = { F_OWNER_TID, (int)mytid };
    long r = sc3(SYS_FCNTL, fds[0], F_SETOWN_EX, (long)&oex);
    check_val("F_SETOWN_EX TID", r, 0);

    uint64_t mask = 1ULL << (SIGUSR1 - 1);
    sc4(SYS_RT_SIGPROCMASK, 0, (long)&mask, 0, 8);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        sc3(SYS_WRITE, fds[1], (long)"x", 1);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    uint64_t info[16] = {0};
    struct k_timespec ts = { 2, 0 };
    long sig = sc4(SYS_RT_SIGTIMEDWAIT, (long)&mask, (long)info, (long)&ts, 8);
    check_val("sigtimedwait → SIGUSR1", sig, SIGUSR1);

    int st = 0;
    sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);

    uint64_t zero = 0;
    sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_UNBLOCK */, (long)&mask, (long)&zero, 8);
}
TEST("ltp/fcntl_owner_tid_signal",     test_fcntl_owner_tid_signal);
TEST("ltp/fcntl_bad_whence_order",     test_fcntl_bad_whence_order);
