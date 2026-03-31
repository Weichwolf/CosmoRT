#include "ktest.h"
#include "cosmort.h"

/* SYS_COSMO_RT_QUERY subcommands */
#define RT_QUERY_IS_RT   0
#define RT_QUERY_CORE_ID 1
#define RT_QUERY_WAKE    2

static void test_ipi_wake(void) {
    puts("\n[IPI Wake]\n");

    /* ── rt_wake: send IPI to core 0 (no-crash) ── */
    long r = sc2(SYS_COSMO_RT_QUERY, RT_QUERY_WAKE, 0);
    check_val("rt_wake(0) no crash", r, 0);

    /* ── rt_wake: send IPI to core 1 (no-crash) ── */
    r = sc2(SYS_COSMO_RT_QUERY, RT_QUERY_WAKE, 1);
    check_val("rt_wake(1) no crash", r, 0);

    /* ── rt_wake: invalid core (silently ignored) ── */
    r = sc2(SYS_COSMO_RT_QUERY, RT_QUERY_WAKE, 99);
    check_val("rt_wake(99) no crash", r, 0);

    /* ── sched_wake via pipe: fork child, child blocks on read,
     *    parent writes to wake it. Exercises the full wake path:
     *    write → pipe has data → blocked reader woken → sched_add →
     *    rt_wake IPI → child resumes on compute core. ── */

    int fds[2] = {-1, -1};
    r = sc1(SYS_PIPE, (long)fds);
    check_val("pipe created", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: close write end, block on read */
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

    /* Parent: close read end, give child time to block on read */
    sc1(SYS_CLOSE, fds[0]);
    struct k_timespec ts = { 0, 50000000 }; /* 50ms */
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Write one byte to wake the child */
    char msg = 42;
    long n = sc3(SYS_WRITE, fds[1], (long)&msg, 1);
    check_val("write to pipe", n, 1);
    sc1(SYS_CLOSE, fds[1]);

    /* Wait for child exit */
    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("child woke and exited 42", (wstatus >> 8) & 0xFF, 42);
}

TEST("ipi_wake", test_ipi_wake);
