#include "ktest.h"

static void test_fd_cloexec(void) {
    puts("\n[FD_CLOEXEC]\n");

    /* Open a file, verify no CLOEXEC by default */
    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open /dev/null >= 0", fd >= 0);
    if (fd < 0) return;

    long fd_flags = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check_val("default: F_GETFD = 0", fd_flags, 0);

    /* Set FD_CLOEXEC via F_SETFD */
    long r = sc3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    check_val("F_SETFD(FD_CLOEXEC) = 0", r, 0);

    fd_flags = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check_val("after set: F_GETFD = 1", fd_flags, FD_CLOEXEC);

    /* Clear FD_CLOEXEC */
    r = sc3(SYS_FCNTL, fd, F_SETFD, 0);
    check_val("F_SETFD(0) = 0", r, 0);

    fd_flags = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check_val("after clear: F_GETFD = 0", fd_flags, 0);

    /* F_SETFL should not clobber O_CLOEXEC */
    sc3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    sc3(SYS_FCNTL, fd, F_SETFL, (long)O_NONBLOCK);
    fd_flags = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check_val("F_SETFL preserves CLOEXEC", fd_flags, FD_CLOEXEC);

    /* F_GETFL should not include O_CLOEXEC */
    long fl = sc3(SYS_FCNTL, fd, F_GETFL, 0);
    check("F_GETFL excludes O_CLOEXEC", (fl & O_CLOEXEC) == 0);
    check("F_GETFL includes O_NONBLOCK", (fl & O_NONBLOCK) != 0);

    sc1(SYS_CLOSE, fd);
}

TEST("fd_cloexec", test_fd_cloexec);
