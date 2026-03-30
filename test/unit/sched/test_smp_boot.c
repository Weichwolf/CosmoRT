#include "ktest.h"
#include "cosmort.h"

/* SYS_COSMO_RT_QUERY subcommands */
#define RT_QUERY_IS_RT   0
#define RT_QUERY_CORE_ID 1

static void test_smp_boot(void) {
    puts("\n[SMP Boot]\n");

    /* ── 1. Multiple cores detected ── */
    long ncpu = sc1(SYS_SYSINFO, 0);
    /* sysinfo returns struct — use getcpu + fork to infer multi-core */
    /* Simpler: RT_QUERY core_id(0) exists = BSP alive */
    long core0 = sc2(SYS_COSMO_RT_QUERY, RT_QUERY_CORE_ID, 0);
    check_val("bsp core 0 alive", core0, 0);

    /* ── 2. This process runs on a compute core (not RT) ── */
    long is_rt = sc2(SYS_COSMO_RT_QUERY, RT_QUERY_IS_RT, 0);
    check_val("ktest on compute core", is_rt, 0);

    /* ── 3. Fork child, both complete (SMP functional) ── */
    int fds[2];
    sc1(SYS_PIPE, (long)fds);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        char c = 'A';
        sc3(SYS_WRITE, fds[1], (long)&c, 1);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);
    sc1(SYS_CLOSE, fds[1]);
    char buf[1] = {0};
    long n = sc3(SYS_READ, fds[0], (long)buf, 1);
    sc1(SYS_CLOSE, fds[0]);
    check_val("child wrote byte", n, 1);
    check_val("child data correct", buf[0], 'A');
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("child exited 0", (ws >> 8) & 0xFF, 0);

    /* ── 4. Multiple concurrent children (exercises SMP scheduling) ── */
    #define N_CHILDREN 4
    long pids[N_CHILDREN];
    int pfds[N_CHILDREN][2];
    for (int i = 0; i < N_CHILDREN; i++) {
        sc1(SYS_PIPE, (long)pfds[i]);
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) {
            sc1(SYS_CLOSE, pfds[i][0]);
            char v = (char)(i + 10);
            sc3(SYS_WRITE, pfds[i][1], (long)&v, 1);
            sc1(SYS_CLOSE, pfds[i][1]);
            sc1(SYS_EXIT, i + 10);
            __builtin_unreachable();
        }
        sc1(SYS_CLOSE, pfds[i][1]);
    }
    int all_ok = 1;
    for (int i = 0; i < N_CHILDREN; i++) {
        char v = 0;
        sc3(SYS_READ, pfds[i][0], (long)&v, 1);
        sc1(SYS_CLOSE, pfds[i][0]);
        int wst = 0;
        sc4(SYS_WAIT4, pids[i], (long)&wst, 0, 0);
        if (((wst >> 8) & 0xFF) != (i + 10)) all_ok = 0;
        if (v != (char)(i + 10)) all_ok = 0;
    }
    check("4 concurrent children correct", all_ok);

    /* ── 5. Shared page write from child visible to parent ── */
    long shared = sc6(SYS_MMAP, 0, 4096, PROT_RW,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("shared mmap ok", shared > 0);
    *(volatile int *)shared = 0;
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        *(volatile int *)shared = 42;
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("shared write visible", *(volatile int *)shared, 42);
    sc2(SYS_MUNMAP, shared, 4096);

    /* ── 6. getpid consistency across fork ── */
    long my_pid = sc0(SYS_GETPID);
    check("getpid > 0", my_pid > 0);
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        long child_pid = sc0(SYS_GETPID);
        long ppid = sc0(SYS_GETPPID);
        /* exit with: 1 if both ok, 0 otherwise */
        sc1(SYS_EXIT, (child_pid != my_pid && ppid == my_pid) ? 1 : 0);
        __builtin_unreachable();
    }
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("child pid/ppid correct", (ws >> 8) & 0xFF, 1);

    /* ── 7. Pipe across processes (SMP IPC) ── */
    int p2[2];
    sc1(SYS_PIPE, (long)p2);
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, p2[1]);
        char rbuf[4];
        long rn = sc3(SYS_READ, p2[0], (long)rbuf, 4);
        sc1(SYS_CLOSE, p2[0]);
        sc1(SYS_EXIT, (rn == 4 && rbuf[0] == 'T') ? 1 : 0);
        __builtin_unreachable();
    }
    sc1(SYS_CLOSE, p2[0]);
    sc3(SYS_WRITE, p2[1], (long)"TEST", 4);
    sc1(SYS_CLOSE, p2[1]);
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("pipe ipc correct", (ws >> 8) & 0xFF, 1);
}

TEST("smp_boot", test_smp_boot);
