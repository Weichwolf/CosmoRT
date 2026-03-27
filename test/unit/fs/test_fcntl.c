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

TEST("fcntl", test_fcntl);
