/* test: dup/dup2 clear O_CLOEXEC per POSIX */
#include "ktest.h"

/* dup() on CLOEXEC fd → new fd has no CLOEXEC */
static void test_dup_clears_cloexec(void) {
    puts("\n[dup: clears cloexec]\n");

    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;

    /* Set CLOEXEC via F_SETFD (reliable path) */
    sc3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    long flags = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check("source has CLOEXEC", (flags & FD_CLOEXEC) != 0);

    long newfd = sc1(SYS_DUP, fd);
    check("dup succeeds", newfd >= 0);
    if (newfd >= 0) {
        flags = sc3(SYS_FCNTL, newfd, F_GETFD, 0);
        check_val("dup clears CLOEXEC", flags & FD_CLOEXEC, 0);
        sc1(SYS_CLOSE, newfd);
    }

    sc1(SYS_CLOSE, fd);
}

/* dup2() on CLOEXEC fd → new fd has no CLOEXEC */
static void test_dup2_clears_cloexec(void) {
    puts("\n[dup2: clears cloexec]\n");

    long fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;

    sc3(SYS_FCNTL, fd, F_SETFD, FD_CLOEXEC);
    long flags = sc3(SYS_FCNTL, fd, F_GETFD, 0);
    check("source has CLOEXEC", (flags & FD_CLOEXEC) != 0);

    long target = 50;
    long r = sc2(SYS_DUP2, fd, target);
    check_val("dup2 returns target", r, target);

    flags = sc3(SYS_FCNTL, target, F_GETFD, 0);
    check_val("dup2 clears CLOEXEC", flags & FD_CLOEXEC, 0);

    sc1(SYS_CLOSE, target);
    sc1(SYS_CLOSE, fd);
}

TEST("dup-clears-cloexec", test_dup_clears_cloexec);
TEST("dup2-clears-cloexec", test_dup2_clears_cloexec);
