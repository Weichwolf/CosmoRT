#include "ktest.h"

static void test_fcntl(void) {
    puts("\n[Fcntl]\n");

    /* Open a file to get a base fd */
    long fd = sc3(SYS_OPEN, (long)"/test_fcntl.tmp", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("open for fcntl test", fd >= 0);
    if (fd < 0) return;

    /* F_DUPFD_CLOEXEC: newfd >= 10 */
    long newfd = sc3(SYS_FCNTL, fd, F_DUPFD_CLOEXEC, 10);
    check_ge("F_DUPFD_CLOEXEC newfd >= 10", newfd, 10);

    /* F_GETFD on newfd → FD_CLOEXEC set */
    if (newfd >= 0) {
        long flags = sc3(SYS_FCNTL, newfd, F_GETFD, 0);
        check("F_GETFD has FD_CLOEXEC", flags & FD_CLOEXEC);
        sc1(SYS_CLOSE, newfd);
    }

    /* F_DUPFD (plain): newfd >= 20, no CLOEXEC */
    newfd = sc3(SYS_FCNTL, fd, F_DUPFD, 20);
    check_ge("F_DUPFD newfd >= 20", newfd, 20);
    if (newfd >= 0) {
        long flags = sc3(SYS_FCNTL, newfd, F_GETFD, 0);
        check("F_DUPFD no CLOEXEC", !(flags & FD_CLOEXEC));
        sc1(SYS_CLOSE, newfd);
    }

    /* F_SETFD / F_GETFD roundtrip */
    sc3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    long g = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check("F_SETFD/F_GETFD roundtrip", g & FD_CLOEXEC);

    /* F_SETLK: advisory write lock → 0 (single-user, always succeeds) */
    struct k_flock fl = { .l_type = F_WRLCK, .l_whence = 0, .l_start = 0, .l_len = 0 };
    long lr = sc3(SYS_FCNTL, fd, F_SETLK, (long)&fl);
    check_val("F_SETLK write lock", lr, 0);

    /* F_GETLK: reports F_UNLCK (no contention in single-user) */
    struct k_flock fl2 = { .l_type = F_WRLCK, .l_whence = 0, .l_start = 0, .l_len = 0 };
    lr = sc3(SYS_FCNTL, fd, F_GETLK, (long)&fl2);
    check_val("F_GETLK returns 0", lr, 0);
    check_val("F_GETLK l_type = F_UNLCK", fl2.l_type, F_UNLCK);

    /* F_SETLKW: blocking lock → 0 */
    struct k_flock fl3 = { .l_type = F_RDLCK, .l_whence = 0, .l_start = 0, .l_len = 0 };
    lr = sc3(SYS_FCNTL, fd, F_SETLKW, (long)&fl3);
    check_val("F_SETLKW read lock", lr, 0);

    sc1(SYS_CLOSE, fd);
}

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/* ── POSIX-Lock wird bei Prozess-Exit freigegeben, nicht erst bei wait4 ── */
static void test_posix_lock_on_exit(void) {
    puts("\n[POSIX_LOCK_ON_EXIT]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/fcntl_exit_lock",
                  O_CREAT | O_RDWR | O_TRUNC, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"hello_world", 11);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid == 0) {
        long cfd = sc3(SYS_OPEN, (long)"/tmp/fcntl_exit_lock", O_RDWR, 0);
        struct k_flock lk = { F_WRLCK, SEEK_SET, 0, 10, 0 };
        sc3(SYS_FCNTL, cfd, F_SETLK, (long)&lk);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* Child has taken lock. Before wait4, parent probes — must see child's lock. */
    for (int spin = 0; spin < 200; spin++) {
        struct k_flock probe = { F_WRLCK, SEEK_SET, 0, 10, 0 };
        sc3(SYS_FCNTL, fd, F_GETLK, (long)&probe);
        if (probe.l_type != F_UNLCK && probe.l_pid == (int)pid) break;
        for (volatile int i = 0; i < 100000; i++) ;
    }

    /* Child exits without close. POSIX locks must be released by
     * exit_kill_process, not only by proc_cleanup (which runs in wait4). */
    for (int spin = 0; spin < 200; spin++) {
        struct k_flock probe = { F_WRLCK, SEEK_SET, 0, 10, 0 };
        sc3(SYS_FCNTL, fd, F_GETLK, (long)&probe);
        if (probe.l_type == F_UNLCK) break;
        for (volatile int i = 0; i < 100000; i++) ;
    }

    struct k_flock final = { F_WRLCK, SEEK_SET, 0, 10, 0 };
    sc3(SYS_FCNTL, fd, F_GETLK, (long)&final);
    check_val("lock released before wait4", final.l_type, F_UNLCK);

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/fcntl_exit_lock");
}

TEST("fcntl", test_fcntl);
TEST("posix_lock_on_exit", test_posix_lock_on_exit);
