#include "ktest.h"

static void test_eventfd_flags(void) {
    puts("\n[eventfd flags]\n");

    /* eventfd2 with EFD_CLOEXEC */
    long efd = sc2(SYS_EVENTFD2, 0, (long)EFD_CLOEXEC);
    check("eventfd2(EFD_CLOEXEC) >= 0", efd >= 0);
    if (efd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, efd, F_GETFD, 0);
        check_val("eventfd CLOEXEC: F_GETFD = 1", fd_flags, FD_CLOEXEC);
        sc1(SYS_CLOSE, efd);
    }

    /* eventfd2 with EFD_NONBLOCK */
    efd = sc2(SYS_EVENTFD2, 0, (long)EFD_NONBLOCK);
    check("eventfd2(EFD_NONBLOCK) >= 0", efd >= 0);
    if (efd >= 0) {
        long fl = sc3(SYS_FCNTL, efd, F_GETFL, 0);
        check("eventfd NONBLOCK: O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        sc1(SYS_CLOSE, efd);
    }

    /* eventfd2 with both flags */
    efd = sc2(SYS_EVENTFD2, 0, (long)(EFD_CLOEXEC | EFD_NONBLOCK));
    check("eventfd2(CLOEXEC|NONBLOCK) >= 0", efd >= 0);
    if (efd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, efd, F_GETFD, 0);
        check_val("eventfd both: F_GETFD = 1", fd_flags, FD_CLOEXEC);
        long fl = sc3(SYS_FCNTL, efd, F_GETFL, 0);
        check("eventfd both: O_NONBLOCK set", (fl & O_NONBLOCK) != 0);
        check("eventfd: F_GETFL excludes CLOEXEC", (fl & O_CLOEXEC) == 0);
        sc1(SYS_CLOSE, efd);
    }

    /* eventfd2 with no flags */
    efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2(0) >= 0", efd >= 0);
    if (efd >= 0) {
        long fd_flags = sc3(SYS_FCNTL, efd, F_GETFD, 0);
        check_val("eventfd(0): F_GETFD = 0", fd_flags, 0);
        sc1(SYS_CLOSE, efd);
    }
}

TEST("eventfd_flags", test_eventfd_flags);
