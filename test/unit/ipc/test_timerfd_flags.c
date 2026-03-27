#include "ktest.h"

static void test_timerfd_flags(void) {
    puts("\n[timerfd flags]\n");

    /* timerfd_create with TFD_CLOEXEC */
    long tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, (long)TFD_CLOEXEC);
    check("timerfd(TFD_CLOEXEC) >= 0", tfd >= 0);
    if (tfd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, tfd, F_GETFD, 0);
        check_val("timerfd CLOEXEC: F_GETFD = 1", fd_flags, FD_CLOEXEC);
        sc1(SYS_CLOSE, tfd);
    }

    /* timerfd_create with TFD_NONBLOCK */
    tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, (long)TFD_NONBLOCK);
    check("timerfd(TFD_NONBLOCK) >= 0", tfd >= 0);
    if (tfd >= 0) {
        long fl = sc3(SYS_FCNTL, tfd, F_GETFL, 0);
        check("timerfd NONBLOCK: O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, tfd);
    }

    /* timerfd with no flags */
    tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
    check("timerfd(0) >= 0", tfd >= 0);
    if (tfd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, tfd, F_GETFD, 0);
        check_val("timerfd(0): F_GETFD = 0", fd_flags, 0);
        sc1(SYS_CLOSE, tfd);
    }
}

TEST("timerfd_flags", test_timerfd_flags);
