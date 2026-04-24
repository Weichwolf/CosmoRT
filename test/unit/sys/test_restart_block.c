/* CosmoRT restart_block ktests
 *
 * Phase 11: signal-restartable syscalls. Exercises ERESTART_RESTARTBLOCK
 * (clock_nanosleep, futex_wait) and ERESTARTSYS (read/wait4/...) via
 * apply_restart() in the syscall-return path.
 *
 * Coverage:
 *   01: nanosleep + SIGUSR1 (handler, no SA_RESTART) -> EINTR + rem updated
 *   02: nanosleep + SIGUSR1 (handler, SA_RESTART)    -> sleep completes (no EINTR)
 *   03: nanosleep + SIG_DFL ignore (SIGCHLD)         -> sleep completes
 *   04: clock_nanosleep TIMER_ABSTIME + SIGINT       -> EINTR at saved deadline
 *   05: futex wait + SIGUSR1 handler                 -> EINTR (handler swallows restart)
 *   06: read on PTY blocked + SIGUSR1                -> EINTR  (no SA_RESTART)
 *   07: wait4 + SIGUSR1 handler (no SA_RESTART)      -> EINTR
 *   08: wait4 + SIGUSR1 handler (SA_RESTART)         -> wait completes after handler
 */

#include "ktest.h"

struct ksigaction_rb {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void rb_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static volatile int rb_sig_received;
static void rb_sig_handler(int sig) { (void)sig; rb_sig_received = 1; }

static void install_handler_flags(int signo, void (*fn)(int), uint64_t extra_flags) {
    struct ksigaction_rb sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER | extra_flags,
        .restorer = (void *)rb_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* ── 01: nanosleep + signal-without-SA_RESTART → EINTR + rem ── */

static void test_rb_01_nanosleep_eintr_rem(void) {
    puts("\n[restart_block]\n");
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_handler_flags(SIGUSR1, rb_sig_handler, 0);
        struct k_timespec rq = { .tv_sec = 1, .tv_nsec = 0 };
        struct k_timespec rem = { .tv_sec = -1, .tv_nsec = -1 };
        long r = sc2(SYS_NANOSLEEP, (long)&rq, (long)&rem);
        /* Encode result: bit 0 = (r==-EINTR), bit 1 = (rem.tv_sec >= 0 && rem.tv_sec <= 1)
         * bit 2 = (rem.tv_sec > 0 || rem.tv_nsec > 0) */
        int code = 0;
        if (r == -EINTR) code |= 1;
        if (rem.tv_sec >= 0 && rem.tv_sec <= 1) code |= 2;
        if (rem.tv_sec > 0 || rem.tv_nsec > 0) code |= 4;
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }
    struct k_timespec d = { .tv_sec = 0, .tv_nsec = 30000000 /* 30ms */ };
    sc2(SYS_NANOSLEEP, (long)&d, 0);
    sc2(SYS_KILL, pid, SIGUSR1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("nanosleep SIGUSR1 -> EINTR",     (code & 1) != 0);
    check("nanosleep rem in [0,1]s",        (code & 2) != 0);
    check("nanosleep rem nonzero",          (code & 4) != 0);
}

/* ── 02: nanosleep + SA_RESTART handler → STILL EINTR ──
 * Linux/x86 do_signal semantics: ERESTART_RESTARTBLOCK is *always*
 * converted to EINTR when a user handler runs, regardless of SA_RESTART.
 * Auto-restart for SA_RESTART is libc-side (glibc does it; musl does not).
 * Kernel-side this means: handler runs, syscall returns EINTR, libc may
 * call SYS_restart_syscall(219) explicitly to resume. */

static void test_rb_02_nanosleep_sa_restart_eintr(void) {
    install_handler_flags(SIGUSR1, rb_sig_handler, SA_RESTART);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_handler_flags(SIGUSR1, rb_sig_handler, SA_RESTART);
        struct k_timespec rq = { .tv_sec = 0, .tv_nsec = 100000000 /* 100ms */ };
        struct k_timespec rem = { .tv_sec = -1, .tv_nsec = -1 };
        long r = sc2(SYS_NANOSLEEP, (long)&rq, (long)&rem);
        /* Expect EINTR + rem updated (RESTARTBLOCK swallowed by handler) */
        int code = 0;
        if (r == -EINTR) code |= 1;
        if (rem.tv_sec >= 0 && rem.tv_nsec >= 0) code |= 2;
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }
    struct k_timespec d = { .tv_sec = 0, .tv_nsec = 20000000 };
    sc2(SYS_NANOSLEEP, (long)&d, 0);
    sc2(SYS_KILL, pid, SIGUSR1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    /* Restore to no-SA_RESTART for subsequent tests */
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    check("nanosleep+SA_RESTART -> EINTR (RESTARTBLOCK swallowed)", (code & 1) != 0);
    check("nanosleep+SA_RESTART rem updated",                       (code & 2) != 0);
}

/* ── 03: nanosleep + default-IGN signal (SIGCHLD) → no EINTR ── */

static void test_rb_03_nanosleep_default_ign(void) {
    /* SIGCHLD with SIG_DFL is "ignore" by default. A pending SIGCHLD must
     * NOT interrupt nanosleep (apply_restart sees no handler -> restart).
     * We trigger SIGCHLD via fork+exit while parent is asleep. */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec d = { .tv_sec = 0, .tv_nsec = 30000000 /* 30ms */ };
        sc2(SYS_NANOSLEEP, (long)&d, 0);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }
    /* Parent: sleep 200ms; child exit at ~30ms sends SIGCHLD (default IGN).
     * Sleep should complete fully (~200ms) without EINTR-restart loop. */
    struct k_timespec rq = { .tv_sec = 0, .tv_nsec = 200000000 };
    struct k_timespec t0, t1;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
    long r = sc2(SYS_NANOSLEEP, (long)&rq, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    check("default-IGN SIGCHLD: nanosleep returns 0", r == 0);
    check("default-IGN SIGCHLD: full 200ms sleep",     elapsed >= 180000000L);
}

/* ── 04: clock_nanosleep TIMER_ABSTIME + SIGUSR1 → EINTR at saved deadline ── */

static void test_rb_04_clock_nanosleep_abstime(void) {
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_handler_flags(SIGUSR1, rb_sig_handler, 0);
        struct k_timespec now;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&now);
        struct k_timespec target = now;
        target.tv_sec += 1;  /* sleep until now+1s */
        long r = sc4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME,
                     (long)&target, 0);
        sc1(SYS_EXIT, r == -EINTR ? 1 : (r == 0 ? 0 : 99));
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }
    struct k_timespec d = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&d, 0);
    sc2(SYS_KILL, pid, SIGUSR1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("clock_nanosleep TIMER_ABSTIME EINTR", code == 1);
}

/* ── 05: futex wait + SIGUSR1 handler → EINTR ── */

static void test_rb_05_futex_wait_signal(void) {
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    static volatile uint32_t fut = 0;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_handler_flags(SIGUSR1, rb_sig_handler, 0);
        /* FUTEX_WAIT_PRIVATE: op=128 (FUTEX_WAIT|FUTEX_PRIVATE_FLAG=128). */
        long r = sc6(SYS_FUTEX, (long)&fut, 0 | 128, 0, 0, 0, 0);
        sc1(SYS_EXIT, r == -EINTR ? 1 : (r == 0 ? 2 : 99));
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }
    struct k_timespec d = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&d, 0);
    sc2(SYS_KILL, pid, SIGUSR1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("futex_wait SIGUSR1 -> EINTR", code == 1);
}

/* ── 06: pipe blocking read + SIGUSR1 → EINTR ── */

static void test_rb_06_pipe_read_eintr(void) {
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    int fds[2];
    if (sc2(SYS_PIPE2, (long)fds, 0) < 0) { fail("pipe2", 0); return; }
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_handler_flags(SIGUSR1, rb_sig_handler, 0);
        sc1(SYS_CLOSE, fds[1]);
        char buf[4];
        long r = sc3(SYS_READ, fds[0], (long)buf, sizeof(buf));
        sc1(SYS_EXIT, r == -EINTR ? 1 : 99);
        __builtin_unreachable();
    }
    if (pid < 0) { sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]); fail("fork", 0); return; }
    sc1(SYS_CLOSE, fds[0]);
    struct k_timespec d = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&d, 0);
    sc2(SYS_KILL, pid, SIGUSR1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    sc1(SYS_CLOSE, fds[1]);
    int code = (ws >> 8) & 0xFF;
    check("pipe read SIGUSR1 -> EINTR", code == 1);
}

/* ── 07: wait4 + SIGUSR1 (no SA_RESTART) → EINTR ── */

static void test_rb_07_wait4_eintr(void) {
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child sleeps long; parent waits, gets interrupted */
        struct k_timespec rq = { .tv_sec = 2, .tv_nsec = 0 };
        sc2(SYS_NANOSLEEP, (long)&rq, 0);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }

    /* Outer: spawn signaler */
    long signaler = sc0(SYS_FORK);
    if (signaler == 0) {
        long parent = sc0(SYS_GETPPID);
        struct k_timespec d = { .tv_sec = 0, .tv_nsec = 50000000 };
        sc2(SYS_NANOSLEEP, (long)&d, 0);
        sc2(SYS_KILL, parent, SIGUSR1);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }

    int ws = 0;
    long r = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    /* On EINTR: r == -EINTR. Then reap signaler + actual child. */
    int got_eintr = (r == -EINTR);
    if (got_eintr) {
        /* Kill child (don't wait full 2s) and reap */
        sc2(SYS_KILL, pid, 9 /* SIGKILL */);
        sc4(SYS_WAIT4, pid, 0, 0, 0);
    }
    sc4(SYS_WAIT4, signaler, 0, 0, 0);
    check("wait4 SIGUSR1 -> EINTR", got_eintr);
}

/* ── 08: wait4 + SIGUSR1 with SA_RESTART → wait completes after handler ── */

static void test_rb_08_wait4_sa_restart(void) {
    install_handler_flags(SIGUSR1, rb_sig_handler, SA_RESTART);
    long child = sc0(SYS_FORK);
    if (child == 0) {
        /* Child exits after 200ms */
        struct k_timespec rq = { .tv_sec = 0, .tv_nsec = 200000000 };
        sc2(SYS_NANOSLEEP, (long)&rq, 0);
        sc1(SYS_EXIT, 42);
        __builtin_unreachable();
    }
    if (child < 0) { fail("fork", 0); return; }

    long signaler = sc0(SYS_FORK);
    if (signaler == 0) {
        long parent = sc0(SYS_GETPPID);
        /* Send 3 signals during the wait window */
        for (int i = 0; i < 3; i++) {
            struct k_timespec d = { .tv_sec = 0, .tv_nsec = 30000000 };
            sc2(SYS_NANOSLEEP, (long)&d, 0);
            sc2(SYS_KILL, parent, SIGUSR1);
        }
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }

    int ws = 0;
    long r = sc4(SYS_WAIT4, child, (long)&ws, 0, 0);
    sc4(SYS_WAIT4, signaler, 0, 0, 0);
    install_handler_flags(SIGUSR1, rb_sig_handler, 0);
    check("wait4 SA_RESTART completes",        r == child);
    check("wait4 SA_RESTART child exit code",  (ws >> 8) == 42);
}

TEST("restart_block/01_nanosleep_eintr_rem",       test_rb_01_nanosleep_eintr_rem);
TEST("restart_block/02_nanosleep_sa_restart",      test_rb_02_nanosleep_sa_restart_eintr);
TEST("restart_block/03_nanosleep_default_ign",     test_rb_03_nanosleep_default_ign);
TEST("restart_block/04_clock_nanosleep_abstime",   test_rb_04_clock_nanosleep_abstime);
TEST("restart_block/05_futex_wait_signal",         test_rb_05_futex_wait_signal);
TEST("restart_block/06_pipe_read_eintr",           test_rb_06_pipe_read_eintr);
TEST("restart_block/07_wait4_eintr",               test_rb_07_wait4_eintr);
TEST("restart_block/08_wait4_sa_restart",          test_rb_08_wait4_sa_restart);
