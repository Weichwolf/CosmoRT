/* Crash test: kill() must respect sig_blocked.
 * A blocked signal must stay pending, not terminate the process.
 * Bug: kill_one() for SIG_DFL fatal signals ignored sig_blocked
 * and immediately terminated the target, even when the signal
 * was blocked on all threads. */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)

static void test_kill_blocked_self(void) {
    puts("\n[kill respects sig_blocked]\n");

    /* Block SIGHUP (1) */
    uint64_t mask = (1ULL << 1);
    sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&mask, 0, 8);

    /* Send SIGHUP to self — must not terminate because it's blocked */
    long r = sc2(SYS_KILL, sc0(SYS_GETPID), 1);
    check_val("kill(self, SIGHUP) returns 0", r, 0);

    /* We survived — signal should be pending */
    check("still alive after blocked SIGHUP", 1);

    /* Verify the pending signal gets delivered on unblock.
     * Fork a child that sends itself SIGHUP while blocked, then unblocks. */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: block SIGHUP, send to self, then unblock → should die */
        uint64_t block = (1ULL << 1);
        sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&block, 0, 8);
        sc2(SYS_KILL, sc0(SYS_GETPID), 1); /* SIGHUP → pending */
        uint64_t none = 0;
        sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&none, 0, 8);
        /* Should not reach here — SIGHUP kills us */
        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child killed by SIGHUP on unblock", WIFSIGNALED(status) && (status & 0x7F) == 1);

    /* Clear our own pending SIGHUP by ignoring it */
    struct { void *handler; unsigned long flags; void *restorer; unsigned long mask; }
        ign = { (void *)1 /* SIG_IGN */, 0, 0, 0 };
    sc4(SYS_RT_SIGACTION, 1, (long)&ign, 0, 8);
    uint64_t unblock = 0;
    sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&unblock, 0, 8);
    /* Restore default handler */
    struct { void *handler; unsigned long flags; void *restorer; unsigned long mask; }
        dfl = { (void *)0 /* SIG_DFL */, 0, 0, 0 };
    sc4(SYS_RT_SIGACTION, 1, (long)&dfl, 0, 8);
}

static void test_kill_blocked_pgrp(void) {
    puts("\n[kill(0, sig) respects sig_blocked]\n");

    /* Fork a child that blocks SIGHUP, sends kill(0, SIGHUP), survives */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Block SIGHUP */
        uint64_t mask = ~((1ULL << 9) | (1ULL << 19));
        sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&mask, 0, 8);

        /* Send SIGHUP to process group */
        sc2(SYS_KILL, 0, 1);

        /* If we're still alive, the kernel respected sig_blocked */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* Parent: block SIGHUP too (so we survive) */
    uint64_t mask = (1ULL << 1);
    sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&mask, 0, 8);

    int status = 0;
    long w = -1;
    int tries = 2000;
    while (tries-- > 0) {
        w = sc4(SYS_WAIT4, pid, (long)&status, 1 /* WNOHANG */, 0);
        if (w > 0) break;
        if (w < 0 && w != -10) break;
        sc0(SYS_SCHED_YIELD);
    }
    if (w == 0) {
        sc2(SYS_KILL, pid, 9);
        w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    }
    check("child exited 0 (survived kill)", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Clean up: ignore SIGHUP, unblock, restore */
    struct { void *handler; unsigned long flags; void *restorer; unsigned long mask; }
        ign = { (void *)1, 0, 0, 0 };
    sc4(SYS_RT_SIGACTION, 1, (long)&ign, 0, 8);
    uint64_t none = 0;
    sc4(SYS_RT_SIGPROCMASK, 2, (long)&none, 0, 8);
    struct { void *handler; unsigned long flags; void *restorer; unsigned long mask; }
        dfl = { (void *)0, 0, 0, 0 };
    sc4(SYS_RT_SIGACTION, 1, (long)&dfl, 0, 8);
}

CRASH_TEST("crash/kill_blocked_self", test_kill_blocked_self);
CRASH_TEST("crash/kill_blocked_pgrp", test_kill_blocked_pgrp);
