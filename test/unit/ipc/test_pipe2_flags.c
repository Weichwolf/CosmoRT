#include "ktest.h"

/* ── pipe2 flags tests ───────────────────────── */

#define SYS_PIPE2   293
#define SYS_FCNTL   72

static void test_pipe2_flags(void) {
    puts("\n[pipe2 flags]\n");

    int fds[2] = {-1, -1};

    /* pipe2 with O_CLOEXEC */
    long r = sc2(SYS_PIPE2, (long)fds, O_CLOEXEC);
    check_val("pipe2 O_CLOEXEC returns 0", r, 0);
    check("pipe2 fds valid", fds[0] >= 0 && fds[1] >= 0);

    /* Check FD_CLOEXEC is set via F_GETFD */
    long fd_flags = sc3(SYS_FCNTL, fds[0], F_GETFD, 0);
    check("pipe2 read FD_CLOEXEC", fd_flags & FD_CLOEXEC);
    fd_flags = sc3(SYS_FCNTL, fds[1], F_GETFD, 0);
    check("pipe2 write FD_CLOEXEC", fd_flags & FD_CLOEXEC);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);

    /* pipe2 with O_NONBLOCK */
    r = sc2(SYS_PIPE2, (long)fds, O_NONBLOCK);
    check_val("pipe2 O_NONBLOCK returns 0", r, 0);

    /* Check O_NONBLOCK is set via F_GETFL */
    long fl_flags = sc3(SYS_FCNTL, fds[0], F_GETFL, 0);
    check("pipe2 read O_NONBLOCK", fl_flags & O_NONBLOCK);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);

    /* pipe2 with no flags — should still work */
    r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("pipe2 no flags returns 0", r, 0);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

TEST("pipe2 flags", test_pipe2_flags);
