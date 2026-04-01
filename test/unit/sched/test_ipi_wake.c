#include "ktest.h"
#include "cosmort.h"

static void test_wake_pipe(void) {
    puts("\n[Wake via pipe]\n");

    /* sched_wake via pipe: fork child, child blocks on read,
     * parent writes to wake it. Exercises the full wake path:
     * write → pipe has data → blocked reader woken → sched_add →
     * child resumes. */

    int fds[2] = {-1, -1};
    long r = sc1(SYS_PIPE, (long)fds);
    check_val("pipe created", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[1]);
        char buf[1];
        long n = sc3(SYS_READ, fds[0], (long)buf, 1);
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_EXIT, (n == 1) ? (long)(unsigned char)buf[0] : 0);
        __builtin_unreachable();
    }
    check("fork succeeded", pid > 0);
    if (pid <= 0) {
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);
        return;
    }

    sc1(SYS_CLOSE, fds[0]);
    struct k_timespec ts = { 0, 50000000 }; /* 50ms */
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    char msg = 42;
    long n = sc3(SYS_WRITE, fds[1], (long)&msg, 1);
    check_val("write to pipe", n, 1);
    sc1(SYS_CLOSE, fds[1]);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("child woke and exited 42", (wstatus >> 8) & 0xFF, 42);
}

TEST("wake_pipe", test_wake_pipe);
