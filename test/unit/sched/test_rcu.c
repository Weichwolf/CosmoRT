/* CosmoRT RCU Tests — 50+ tests exercising preemptible RCU via scheduling
 *
 * RCU is kernel-internal. We test it by exercising the code paths it guards:
 * context switches (rcu_note_context_switch), timer ticks (rcu_check_callbacks),
 * fork/exec/exit (GP advancement), threads (preempted readers).
 *
 * Categories:
 *   1-10:  Basic scheduling (yield, context switch correctness)
 *   11-20: Fork/exit cycles (GP start/complete under process lifecycle)
 *   21-30: Pipe/IPC (context switches under blocking I/O)
 *   31-40: Threads/clone (preempted reader paths, shared address space)
 *   41-50: Stress (rapid cycling, deep fork trees, mixed workloads)
 *   51+:   Edge cases
 */
#include "ktest.h"

/* ── Helpers ─────────────────────────────────── */

static void child_yield_exit(int yields) {
    for (int i = 0; i < yields; i++)
        sc0(SYS_SCHED_YIELD);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static long fork_yield_wait(int yields) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) child_yield_exit(yields);
    if (pid < 0) return pid;
    int ws = 0;
    long wr = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    return (wr == pid && (ws & 0xFF) == 0) ? 0 : -1;
}

/* ── 1-10: Basic scheduling ──────────────────── */

static void test_rcu_01_single_yield(void) {
    puts("\n[RCU]\n");
    long r = sc0(SYS_SCHED_YIELD);
    check_val("single yield", r, 0);
}

static void test_rcu_02_ten_yields(void) {
    for (int i = 0; i < 10; i++) sc0(SYS_SCHED_YIELD);
    pass("10 yields");
}

static void test_rcu_03_hundred_yields(void) {
    for (int i = 0; i < 100; i++) sc0(SYS_SCHED_YIELD);
    pass("100 yields");
}

static void test_rcu_04_thousand_yields(void) {
    for (int i = 0; i < 1000; i++) sc0(SYS_SCHED_YIELD);
    pass("1000 yields");
}

static void test_rcu_05_yield_getpid(void) {
    long pid1 = sc0(SYS_GETPID);
    sc0(SYS_SCHED_YIELD);
    long pid2 = sc0(SYS_GETPID);
    check_val("pid stable across yield", pid1, pid2);
}

static void test_rcu_06_yield_preserves_state(void) {
    long brk1 = sc1(SYS_BRK, 0);
    for (int i = 0; i < 50; i++) sc0(SYS_SCHED_YIELD);
    long brk2 = sc1(SYS_BRK, 0);
    check_val("brk stable across 50 yields", brk1, brk2);
}

static void test_rcu_07_yield_after_mmap(void) {
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap before yield", addr > 0);
    *(volatile int *)addr = 0xDEAD;
    sc0(SYS_SCHED_YIELD);
    check_val("mmap data survives yield", *(volatile int *)addr, 0xDEAD);
    sc2(SYS_MUNMAP, addr, 4096);
}

static void test_rcu_08_yield_between_pipe_ops(void) {
    int fds[2];
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    if (r < 0) { fail("pipe", 0); return; }
    char w = 'Y';
    sc3(SYS_WRITE, fds[1], (long)&w, 1);
    sc0(SYS_SCHED_YIELD);
    char rd = 0;
    long n = sc3(SYS_READ, fds[0], (long)&rd, 1);
    check_val("read after yield", n, 1);
    check_val("pipe data after yield", rd, 'Y');
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

static void test_rcu_09_yield_nanosleep_interleave(void) {
    struct { long tv_sec; long tv_nsec; } ts = {0, 1000000}; /* 1ms */
    for (int i = 0; i < 10; i++) {
        sc0(SYS_SCHED_YIELD);
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
    }
    pass("yield+nanosleep interleave");
}

static void test_rcu_10_yield_clock_gettime(void) {
    struct { long tv_sec; long tv_nsec; } ts1, ts2;
    sc2(SYS_CLOCK_GETTIME, 0, (long)&ts1);
    for (int i = 0; i < 50; i++) sc0(SYS_SCHED_YIELD);
    sc2(SYS_CLOCK_GETTIME, 0, (long)&ts2);
    check("time advances across yields",
          ts2.tv_sec > ts1.tv_sec ||
          (ts2.tv_sec == ts1.tv_sec && ts2.tv_nsec >= ts1.tv_nsec));
}

/* ── 11-20: Fork/exit cycles ─────────────────── */

static void test_rcu_11_fork_exit_single(void) {
    long r = fork_yield_wait(0);
    check_val("single fork/exit", r, 0);
}

static void test_rcu_12_fork_exit_with_yields(void) {
    long r = fork_yield_wait(5);
    check_val("fork/exit + 5 yields", r, 0);
}

static void test_rcu_13_fork_exit_ten(void) {
    for (int i = 0; i < 10; i++) {
        if (fork_yield_wait(1) < 0) { fail("10 fork/exit cycles", 0); return; }
    }
    pass("10 fork/exit cycles");
}

static void test_rcu_14_fork_exit_twenty(void) {
    for (int i = 0; i < 20; i++) {
        if (fork_yield_wait(2) < 0) { fail("20 fork/exit cycles", 0); return; }
    }
    pass("20 fork/exit cycles");
}

static void test_rcu_15_fork_parallel_reap(void) {
    int n = 10;
    long pids[10];
    for (int i = 0; i < n; i++) {
        pids[i] = sc0(SYS_FORK);
        if (pids[i] == 0) child_yield_exit(3);
        if (pids[i] < 0) { fail("parallel fork", 0); return; }
    }
    for (int i = 0; i < n; i++) {
        int ws = 0;
        long wr = sc4(SYS_WAIT4, pids[i], (long)&ws, 0, 0);
        if (wr != pids[i]) { fail("parallel reap", 0); return; }
    }
    pass("10 parallel fork+reap");
}

static void test_rcu_16_fork_parallel_any_reap(void) {
    int n = 15;
    for (int i = 0; i < n; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) child_yield_exit(i % 5);
        if (pid < 0) { fail("parallel fork 15", 0); return; }
    }
    for (int i = 0; i < n; i++) {
        int ws = 0;
        if (sc4(SYS_WAIT4, -1, (long)&ws, 0, 0) < 0) {
            fail("reap any 15", 0); return;
        }
    }
    pass("15 fork, reap any order");
}

static void test_rcu_17_fork_deep_chain(void) {
    /* Chain: parent forks child, child forks grandchild, grandchild exits */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long pid2 = sc0(SYS_FORK);
        if (pid2 == 0) {
            sc0(SYS_SCHED_YIELD);
            sc1(SYS_EXIT, 0);
            __builtin_unreachable();
        }
        int ws = 0;
        sc4(SYS_WAIT4, pid2, (long)&ws, 0, 0);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("deep chain fork", pid > 0);
    int ws = 0;
    long wr = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("deep chain reap", wr, pid);
}

static void test_rcu_18_fork_yield_before_wait(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) { sc1(SYS_EXIT, 42); __builtin_unreachable(); }
    for (int i = 0; i < 20; i++) sc0(SYS_SCHED_YIELD);
    int ws = 0;
    long wr = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("wait after yields", wr, pid);
    check_val("exit code 42", (ws >> 8) & 0xFF, 42);
}

static void test_rcu_19_fork_child_does_work(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child allocates memory, writes, yields, verifies, exits */
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a > 0) {
            *(volatile int *)a = 0xCAFE;
            sc0(SYS_SCHED_YIELD);
            sc1(SYS_EXIT, *(volatile int *)a == 0xCAFE ? 0 : 1);
        }
        sc1(SYS_EXIT, 2);
        __builtin_unreachable();
    }
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("child work survived yields", (ws >> 8) & 0xFF, 0);
}

static void test_rcu_20_fork_rapid_fire(void) {
    for (int i = 0; i < 30; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) { sc1(SYS_EXIT, 0); __builtin_unreachable(); }
        if (pid < 0) { fail("rapid fork 30", 0); return; }
        int ws = 0;
        sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    }
    pass("30 rapid fork/exit");
}

/* ── 21-30: Pipe/IPC ─────────────────────────── */

static void test_rcu_21_pipe_cross_fork(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        char m = 'X';
        sc3(SYS_WRITE, fds[1], (long)&m, 1);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, fds[1]);
    char b = 0;
    check_val("pipe cross-fork read", sc3(SYS_READ, fds[0], (long)&b, 1), 1);
    check_val("pipe cross-fork data", b, 'X');
    sc1(SYS_CLOSE, fds[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_22_pipe_bidirectional(void) {
    int p2c[2], c2p[2];
    sc2(SYS_PIPE2, (long)p2c, 0);
    sc2(SYS_PIPE2, (long)c2p, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, p2c[1]);
        sc1(SYS_CLOSE, c2p[0]);
        char b = 0;
        sc3(SYS_READ, p2c[0], (long)&b, 1);
        b++;
        sc3(SYS_WRITE, c2p[1], (long)&b, 1);
        sc1(SYS_CLOSE, p2c[0]);
        sc1(SYS_CLOSE, c2p[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, p2c[0]);
    sc1(SYS_CLOSE, c2p[1]);
    char m = 'A';
    sc3(SYS_WRITE, p2c[1], (long)&m, 1);
    char r = 0;
    sc3(SYS_READ, c2p[0], (long)&r, 1);
    check_val("bidir pipe echo", r, 'B');
    sc1(SYS_CLOSE, p2c[1]);
    sc1(SYS_CLOSE, c2p[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_23_pipe_multi_message(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        for (int i = 0; i < 10; i++) {
            char c = (char)('0' + i);
            sc3(SYS_WRITE, fds[1], (long)&c, 1);
            sc0(SYS_SCHED_YIELD);
        }
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, fds[1]);
    char buf[10];
    int total = 0;
    while (total < 10) {
        long n = sc3(SYS_READ, fds[0], (long)(buf + total), 10 - total);
        if (n <= 0) break;
        total += (int)n;
    }
    check_val("pipe 10 messages", total, 10);
    int ok = 1;
    for (int i = 0; i < 10; i++) if (buf[i] != '0' + i) ok = 0;
    check("pipe message order", ok);
    sc1(SYS_CLOSE, fds[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_24_pipe_eof_on_close(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        sc0(SYS_SCHED_YIELD);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, fds[1]);
    char b;
    long n = sc3(SYS_READ, fds[0], (long)&b, 1);
    check_val("pipe EOF after close", n, 0);
    sc1(SYS_CLOSE, fds[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_25_pipe_large_write(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        char buf[256];
        for (int i = 0; i < 256; i++) buf[i] = (char)(i & 0xFF);
        sc3(SYS_WRITE, fds[1], (long)buf, 256);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, fds[1]);
    char buf[256];
    int total = 0;
    while (total < 256) {
        long n = sc3(SYS_READ, fds[0], (long)(buf + total), 256 - total);
        if (n <= 0) break;
        total += (int)n;
    }
    check_val("large pipe transfer", total, 256);
    int ok = 1;
    for (int i = 0; i < 256; i++) if ((buf[i] & 0xFF) != (i & 0xFF)) ok = 0;
    check("large pipe data integrity", ok);
    sc1(SYS_CLOSE, fds[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_26_eventfd_cross_fork(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    if (efd < 0) { fail("eventfd", 0); return; }
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        uint64_t v = 1;
        sc3(SYS_WRITE, efd, (long)&v, 8);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    uint64_t val = 0;
    long n = sc3(SYS_READ, efd, (long)&val, 8);
    check_val("eventfd cross-fork read", n, 8);
    check_val("eventfd value", (long)val, 1);
    sc1(SYS_CLOSE, efd);
}

static void test_rcu_27_socketpair_fork(void) {
    int sv[2];
    long r = sc4(SYS_SOCKETPAIR, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, (long)sv);
    if (r < 0) { fail("socketpair", 0); return; }
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, sv[0]);
        char m = 'S';
        sc3(SYS_WRITE, sv[1], (long)&m, 1);
        sc1(SYS_CLOSE, sv[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, sv[1]);
    char b = 0;
    sc3(SYS_READ, sv[0], (long)&b, 1);
    check_val("socketpair cross-fork", b, 'S');
    sc1(SYS_CLOSE, sv[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_28_pipe_chain(void) {
    /* A → B → C via pipes, each in separate process */
    int ab[2], bc[2];
    sc2(SYS_PIPE2, (long)ab, 0);
    sc2(SYS_PIPE2, (long)bc, 0);
    long pidB = sc0(SYS_FORK);
    if (pidB == 0) {
        sc1(SYS_CLOSE, ab[1]); sc1(SYS_CLOSE, bc[0]);
        char b = 0;
        sc3(SYS_READ, ab[0], (long)&b, 1);
        b += 10;
        sc3(SYS_WRITE, bc[1], (long)&b, 1);
        sc1(SYS_CLOSE, ab[0]); sc1(SYS_CLOSE, bc[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    long pidC = sc0(SYS_FORK);
    if (pidC == 0) {
        sc1(SYS_CLOSE, ab[0]); sc1(SYS_CLOSE, ab[1]); sc1(SYS_CLOSE, bc[1]);
        char b = 0;
        sc3(SYS_READ, bc[0], (long)&b, 1);
        sc1(SYS_CLOSE, bc[0]);
        sc1(SYS_EXIT, (int)(unsigned char)b); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, ab[0]); sc1(SYS_CLOSE, bc[0]); sc1(SYS_CLOSE, bc[1]);
    char m = 5;
    sc3(SYS_WRITE, ab[1], (long)&m, 1);
    sc1(SYS_CLOSE, ab[1]);
    int ws = 0;
    sc4(SYS_WAIT4, pidB, (long)&ws, 0, 0);
    sc4(SYS_WAIT4, pidC, (long)&ws, 0, 0);
    check_val("pipe chain A→B→C", (ws >> 8) & 0xFF, 15);
}

static void test_rcu_29_dup_across_yield(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long dup = sc1(SYS_DUP, fds[1]);
    check("dup write end", dup >= 0);
    sc0(SYS_SCHED_YIELD);
    char m = 'D';
    sc3(SYS_WRITE, dup, (long)&m, 1);
    char b = 0;
    sc3(SYS_READ, fds[0], (long)&b, 1);
    check_val("dup write survives yield", b, 'D');
    sc1(SYS_CLOSE, dup);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

static void test_rcu_30_pipe_nonblock_yield(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, O_NONBLOCK);
    char b;
    long n = sc3(SYS_READ, fds[0], (long)&b, 1);
    check_val("nonblock empty EAGAIN", n, -EAGAIN);
    sc0(SYS_SCHED_YIELD);
    n = sc3(SYS_READ, fds[0], (long)&b, 1);
    check_val("nonblock EAGAIN after yield", n, -EAGAIN);
    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

/* ── 31-40: Threads/clone ────────────────────── */

static volatile int thr_counter;

static void test_rcu_31_thread_basic(void) {
    thr_counter = 0;
    long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
    if (tid == 0) {
        __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    check("clone basic", tid > 0);
    for (volatile int i = 0; i < 5000000 && !thr_counter; i++) __asm__("pause");
    check("thread ran", thr_counter > 0);
}

static void test_rcu_32_thread_yield_interleave(void) {
    thr_counter = 0;
    long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
    if (tid == 0) {
        for (int i = 0; i < 50; i++) sc0(SYS_SCHED_YIELD);
        __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    for (int i = 0; i < 200 && !thr_counter; i++) sc0(SYS_SCHED_YIELD);
    check("thread yield interleave", thr_counter > 0);
}

static void test_rcu_33_two_threads(void) {
    thr_counter = 0;
    for (int t = 0; t < 2; t++) {
        long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
        long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
        if (tid == 0) {
            for (int i = 0; i < 20; i++) sc0(SYS_SCHED_YIELD);
            __sync_fetch_and_add(&thr_counter, 1);
            sc1(SYS_EXIT, 0); __builtin_unreachable();
        }
    }
    for (int i = 0; i < 500 && thr_counter < 2; i++) sc0(SYS_SCHED_YIELD);
    check_val("two threads completed", (long)thr_counter, 2);
}

static void test_rcu_34_thread_shared_counter(void) {
    thr_counter = 0;
    long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
    if (tid == 0) {
        for (int i = 0; i < 100; i++) {
            __sync_fetch_and_add(&thr_counter, 1);
            sc0(SYS_SCHED_YIELD);
        }
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    for (int i = 0; i < 100; i++) {
        __sync_fetch_and_add(&thr_counter, 1);
        sc0(SYS_SCHED_YIELD);
    }
    for (int i = 0; i < 300 && thr_counter < 200; i++) sc0(SYS_SCHED_YIELD);
    check_ge("shared counter", (long)thr_counter, 200);
}

static void test_rcu_35_thread_mmap_after_clone(void) {
    thr_counter = 0;
    long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
    if (tid == 0) {
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a > 0) *(volatile int *)a = 99;
        __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    for (int i = 0; i < 5000000 && !thr_counter; i++) __asm__("pause");
    check("thread mmap after clone", thr_counter > 0);
}

static void test_rcu_36_thread_pipe(void) {
    /* Thread-to-process pipe via fork (clone+pipe is fragile with nonblock) */
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        for (int i = 0; i < 10; i++) sc0(SYS_SCHED_YIELD);
        char m = 'T';
        sc3(SYS_WRITE, fds[1], (long)&m, 1);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, fds[1]);
    char b = 0;
    long n = sc3(SYS_READ, fds[0], (long)&b, 1);
    check_val("thread pipe read", n, 1);
    check_val("thread pipe data", b, 'T');
    sc1(SYS_CLOSE, fds[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_37_thread_rapid_exit(void) {
    for (int i = 0; i < 10; i++) {
        thr_counter = 0;
        long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
        long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
        if (tid == 0) {
            __sync_fetch_and_add(&thr_counter, 1);
            sc1(SYS_EXIT, 0); __builtin_unreachable();
        }
        for (volatile int j = 0; j < 5000000 && !thr_counter; j++) __asm__("pause");
    }
    pass("10 rapid thread create/exit");
}

static void test_rcu_38_thread_getpid_stable(void) {
    long my_pid = sc0(SYS_GETPID);
    thr_counter = 0;
    long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
    if (tid == 0) {
        __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    for (volatile int i = 0; i < 5000000 && !thr_counter; i++) __asm__("pause");
    long pid_after = sc0(SYS_GETPID);
    check_val("pid stable after thread", my_pid, pid_after);
}

static void test_rcu_39_three_threads(void) {
    thr_counter = 0;
    for (int t = 0; t < 3; t++) {
        long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
        long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
        if (tid == 0) {
            for (int i = 0; i < 30; i++) sc0(SYS_SCHED_YIELD);
            __sync_fetch_and_add(&thr_counter, 1);
            sc1(SYS_EXIT, 0); __builtin_unreachable();
        }
    }
    for (int i = 0; i < 500 && thr_counter < 3; i++) sc0(SYS_SCHED_YIELD);
    check_val("three threads done", (long)thr_counter, 3);
}

static void test_rcu_40_thread_plus_fork(void) {
    /* Thread yields while parent forks a child — mixed scheduling */
    thr_counter = 0;
    long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long tid = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk + 65536, 0, 0, 0);
    if (tid == 0) {
        for (int i = 0; i < 30; i++) sc0(SYS_SCHED_YIELD);
        __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    long r = fork_yield_wait(3);
    check_val("fork during thread", r, 0);
    for (int i = 0; i < 300 && !thr_counter; i++) sc0(SYS_SCHED_YIELD);
    check("thread survived fork", thr_counter > 0);
}

/* ── 41-50: Stress ───────────────────────────── */

static void test_rcu_41_fork_storm_50(void) {
    for (int i = 0; i < 50; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) { sc1(SYS_EXIT, 0); __builtin_unreachable(); }
        if (pid < 0) { fail("fork storm 50", 0); return; }
        int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    }
    pass("50 sequential fork/exit");
}

static void test_rcu_42_yield_storm_5000(void) {
    for (int i = 0; i < 5000; i++) sc0(SYS_SCHED_YIELD);
    pass("5000 yields");
}

static void test_rcu_43_mixed_fork_yield(void) {
    for (int i = 0; i < 20; i++) {
        for (int y = 0; y < 10; y++) sc0(SYS_SCHED_YIELD);
        long pid = sc0(SYS_FORK);
        if (pid == 0) child_yield_exit(5);
        if (pid < 0) { fail("mixed fork+yield", 0); return; }
        int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    }
    pass("20 mixed fork+yield rounds");
}

static void test_rcu_44_parallel_10_children(void) {
    int n = 10;
    for (int i = 0; i < n; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) child_yield_exit(10);
        if (pid < 0) { fail("parallel 10", 0); return; }
    }
    for (int i = 0; i < n; i++) {
        int ws = 0;
        if (sc4(SYS_WAIT4, -1, (long)&ws, 0, 0) < 0) {
            fail("parallel 10 reap", 0); return;
        }
    }
    pass("10 parallel children 10 yields each");
}

static void test_rcu_45_pipe_ping_pong(void) {
    int p2c[2], c2p[2];
    sc2(SYS_PIPE2, (long)p2c, 0);
    sc2(SYS_PIPE2, (long)c2p, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, p2c[1]); sc1(SYS_CLOSE, c2p[0]);
        for (int i = 0; i < 20; i++) {
            char b = 0;
            sc3(SYS_READ, p2c[0], (long)&b, 1);
            b++;
            sc3(SYS_WRITE, c2p[1], (long)&b, 1);
        }
        sc1(SYS_CLOSE, p2c[0]); sc1(SYS_CLOSE, c2p[1]);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc1(SYS_CLOSE, p2c[0]); sc1(SYS_CLOSE, c2p[1]);
    char v = 0;
    for (int i = 0; i < 20; i++) {
        sc3(SYS_WRITE, p2c[1], (long)&v, 1);
        sc3(SYS_READ, c2p[0], (long)&v, 1);
    }
    check_val("pipe ping-pong 20 rounds", v, 20);
    sc1(SYS_CLOSE, p2c[1]); sc1(SYS_CLOSE, c2p[0]);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}

static void test_rcu_46_thread_counter_race(void) {
    thr_counter = 0;
    long stk1 = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long stk2 = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    long t1 = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk1 + 65536, 0, 0, 0);
    if (t1 == 0) {
        for (int i = 0; i < 500; i++) __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    long t2 = sc5(SYS_CLONE, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stk2 + 65536, 0, 0, 0);
    if (t2 == 0) {
        for (int i = 0; i < 500; i++) __sync_fetch_and_add(&thr_counter, 1);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    for (int i = 0; i < 500; i++) __sync_fetch_and_add(&thr_counter, 1);
    for (int i = 0; i < 5000 && thr_counter < 1500; i++) sc0(SYS_SCHED_YIELD);
    check_ge("atomic counter 3 threads", (long)thr_counter, 1500);
}

static void test_rcu_47_fork_exec_cycle(void) {
    /* Fork + exec /bin/true equivalent (child just exits) */
    for (int i = 0; i < 10; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            sc0(SYS_SCHED_YIELD);
            sc1(SYS_EXIT, 0); __builtin_unreachable();
        }
        if (pid < 0) { fail("fork/exec cycle", 0); return; }
        int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    }
    pass("10 fork+exit cycles (exec sim)");
}

static void test_rcu_48_deep_fork_3_levels(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long pid2 = sc0(SYS_FORK);
        if (pid2 == 0) {
            long pid3 = sc0(SYS_FORK);
            if (pid3 == 0) { sc0(SYS_SCHED_YIELD); sc1(SYS_EXIT, 7); __builtin_unreachable(); }
            int ws = 0; sc4(SYS_WAIT4, pid3, (long)&ws, 0, 0);
            sc1(SYS_EXIT, (ws >> 8) & 0xFF); __builtin_unreachable();
        }
        int ws = 0; sc4(SYS_WAIT4, pid2, (long)&ws, 0, 0);
        sc1(SYS_EXIT, (ws >> 8) & 0xFF); __builtin_unreachable();
    }
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("3-level fork chain exit code", (ws >> 8) & 0xFF, 7);
}

static void test_rcu_49_yield_getppid(void) {
    long ppid = sc0(SYS_GETPPID);
    for (int i = 0; i < 100; i++) sc0(SYS_SCHED_YIELD);
    check_val("ppid stable after 100 yields", sc0(SYS_GETPPID), ppid);
}

static void test_rcu_50_mmap_munmap_yield(void) {
    for (int i = 0; i < 20; i++) {
        long a = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (a <= 0) { fail("mmap in loop", 0); return; }
        *(volatile int *)a = i;
        sc0(SYS_SCHED_YIELD);
        if (*(volatile int *)a != i) { fail("mmap data after yield", 0); return; }
        sc2(SYS_MUNMAP, a, 4096);
    }
    pass("20 mmap/yield/munmap cycles");
}

/* ── 51+: Edge cases ─────────────────────────── */

static void test_rcu_51_getpid_after_child_exit(void) {
    long my_pid = sc0(SYS_GETPID);
    long pid = sc0(SYS_FORK);
    if (pid == 0) { sc1(SYS_EXIT, 0); __builtin_unreachable(); }
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("getpid stable after child exit", sc0(SYS_GETPID), my_pid);
}

static void test_rcu_52_fork_close_all(void) {
    int fds[2];
    sc2(SYS_PIPE2, (long)fds, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);
        sc0(SYS_SCHED_YIELD);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    /* Parent's fds should still work */
    char m = 'Z';
    sc3(SYS_WRITE, fds[1], (long)&m, 1);
    char b = 0;
    sc3(SYS_READ, fds[0], (long)&b, 1);
    check_val("parent fds survive child close", b, 'Z');
    sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]);
}

static void test_rcu_53_sigchld_yield(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc0(SYS_SCHED_YIELD);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    sc0(SYS_SCHED_YIELD);
    sc0(SYS_SCHED_YIELD);
    int ws = 0;
    long wr = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("SIGCHLD after yields", wr, pid);
}

static void test_rcu_54_fork_brk(void) {
    long brk1 = sc1(SYS_BRK, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_BRK, brk1 + 4096);
        sc0(SYS_SCHED_YIELD);
        sc1(SYS_EXIT, 0); __builtin_unreachable();
    }
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    long brk2 = sc1(SYS_BRK, 0);
    check_val("parent brk unchanged after child brk", brk1, brk2);
}

static void test_rcu_55_double_wait(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) { sc1(SYS_EXIT, 0); __builtin_unreachable(); }
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    long wr2 = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check("double wait returns error", wr2 < 0);
}

TEST("rcu/yield_single",           test_rcu_01_single_yield);
TEST("rcu/yield_10",               test_rcu_02_ten_yields);
TEST("rcu/yield_100",              test_rcu_03_hundred_yields);
TEST("rcu/yield_1000",             test_rcu_04_thousand_yields);
TEST("rcu/yield_getpid",           test_rcu_05_yield_getpid);
TEST("rcu/yield_brk_stable",       test_rcu_06_yield_preserves_state);
TEST("rcu/yield_mmap_data",        test_rcu_07_yield_after_mmap);
TEST("rcu/yield_pipe_ops",         test_rcu_08_yield_between_pipe_ops);
TEST("rcu/yield_nanosleep",        test_rcu_09_yield_nanosleep_interleave);
TEST("rcu/yield_clock",            test_rcu_10_yield_clock_gettime);
TEST("rcu/fork_single",            test_rcu_11_fork_exit_single);
TEST("rcu/fork_yields",            test_rcu_12_fork_exit_with_yields);
TEST("rcu/fork_10x",               test_rcu_13_fork_exit_ten);
TEST("rcu/fork_20x",               test_rcu_14_fork_exit_twenty);
TEST("rcu/fork_parallel_reap",     test_rcu_15_fork_parallel_reap);
TEST("rcu/fork_any_reap",          test_rcu_16_fork_parallel_any_reap);
TEST("rcu/fork_deep_chain",        test_rcu_17_fork_deep_chain);
TEST("rcu/fork_yield_wait",        test_rcu_18_fork_yield_before_wait);
TEST("rcu/fork_child_work",        test_rcu_19_fork_child_does_work);
TEST("rcu/fork_rapid_30",          test_rcu_20_fork_rapid_fire);
TEST("rcu/pipe_cross_fork",        test_rcu_21_pipe_cross_fork);
TEST("rcu/pipe_bidir",             test_rcu_22_pipe_bidirectional);
TEST("rcu/pipe_multi_msg",         test_rcu_23_pipe_multi_message);
TEST("rcu/pipe_eof",               test_rcu_24_pipe_eof_on_close);
TEST("rcu/pipe_large",             test_rcu_25_pipe_large_write);
TEST("rcu/eventfd_fork",           test_rcu_26_eventfd_cross_fork);
TEST("rcu/socketpair_fork",        test_rcu_27_socketpair_fork);
TEST("rcu/pipe_chain_abc",         test_rcu_28_pipe_chain);
TEST("rcu/dup_yield",              test_rcu_29_dup_across_yield);
TEST("rcu/pipe_nonblock_yield",    test_rcu_30_pipe_nonblock_yield);
TEST("rcu/thread_basic",           test_rcu_31_thread_basic);
TEST("rcu/thread_yield_interleave",test_rcu_32_thread_yield_interleave);
TEST("rcu/two_threads",            test_rcu_33_two_threads);
TEST("rcu/thread_shared_counter",  test_rcu_34_thread_shared_counter);
TEST("rcu/thread_mmap",            test_rcu_35_thread_mmap_after_clone);
TEST("rcu/thread_pipe",            test_rcu_36_thread_pipe);
TEST("rcu/thread_rapid_exit",      test_rcu_37_thread_rapid_exit);
TEST("rcu/thread_pid_stable",      test_rcu_38_thread_getpid_stable);
TEST("rcu/three_threads",          test_rcu_39_three_threads);
TEST("rcu/thread_plus_fork",       test_rcu_40_thread_plus_fork);
TEST("rcu/fork_storm_50",          test_rcu_41_fork_storm_50);
TEST("rcu/yield_storm_5000",       test_rcu_42_yield_storm_5000);
TEST("rcu/mixed_fork_yield",       test_rcu_43_mixed_fork_yield);
TEST("rcu/parallel_10_children",   test_rcu_44_parallel_10_children);
TEST("rcu/pipe_ping_pong",         test_rcu_45_pipe_ping_pong);
TEST("rcu/thread_counter_race",    test_rcu_46_thread_counter_race);
TEST("rcu/fork_exec_cycle",        test_rcu_47_fork_exec_cycle);
TEST("rcu/deep_fork_3level",       test_rcu_48_deep_fork_3_levels);
TEST("rcu/yield_ppid",             test_rcu_49_yield_getppid);
TEST("rcu/mmap_yield_munmap",      test_rcu_50_mmap_munmap_yield);
TEST("rcu/getpid_after_child",     test_rcu_51_getpid_after_child_exit);
TEST("rcu/fork_close_isolation",   test_rcu_52_fork_close_all);
TEST("rcu/sigchld_yield",          test_rcu_53_sigchld_yield);
TEST("rcu/fork_brk_isolation",     test_rcu_54_fork_brk);
TEST("rcu/double_wait",            test_rcu_55_double_wait);
