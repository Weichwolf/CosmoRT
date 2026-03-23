#include "ktest.h"

static void test_fcntl(void) {
    puts("\n[Fcntl]\n");

    /* Open a file to get a base fd */
    long fd = sc3(SYS_open, (long)"/test_fcntl.tmp", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("open for fcntl test", fd >= 0);
    if (fd < 0) return;

    /* F_DUPFD_CLOEXEC: newfd >= 10 */
    long newfd = sc3(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 10);
    check_ge("F_DUPFD_CLOEXEC newfd >= 10", newfd, 10);

    /* F_GETFD on newfd → FD_CLOEXEC set */
    if (newfd >= 0) {
        long flags = sc3(SYS_fcntl, newfd, F_GETFD, 0);
        check("F_GETFD has FD_CLOEXEC", flags & FD_CLOEXEC);
        sc1(SYS_close, newfd);
    }

    /* F_DUPFD (plain): newfd >= 20, no CLOEXEC */
    newfd = sc3(SYS_fcntl, fd, F_DUPFD, 20);
    check_ge("F_DUPFD newfd >= 20", newfd, 20);
    if (newfd >= 0) {
        long flags = sc3(SYS_fcntl, newfd, F_GETFD, 0);
        check("F_DUPFD no CLOEXEC", !(flags & FD_CLOEXEC));
        sc1(SYS_close, newfd);
    }

    /* F_SETFD / F_GETFD roundtrip */
    sc3(SYS_fcntl, fd, F_SETFD, FD_CLOEXEC);
    long g = sc3(SYS_fcntl, fd, F_GETFD, 0);
    check("F_SETFD/F_GETFD roundtrip", g & FD_CLOEXEC);

    sc1(SYS_close, fd);
}

TEST("fcntl", test_fcntl);
