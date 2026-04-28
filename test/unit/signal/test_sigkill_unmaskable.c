/* SIGKILL must penetrate every blocked-sleep path.
 *
 * Track 1 architecture invariant: no thread can mask SIGKILL or SIGSTOP,
 * and a thread parked in any *_INTERRUPTIBLE / *_KILLABLE sleep must wake
 * promptly when SIGKILL arrives, even if rt_sigprocmask was given a mask
 * that included SIGKILL/SIGSTOP (which Linux silently strips).
 */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

static void busy_wait_ms(int ms) {
    struct k_timespec ts = { 0, (long)ms * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* Try to set SIGKILL+SIGSTOP into the blocked mask. Linux strips them
 * silently — sigprocmask returns 0, but the bits are not in sig_blocked.
 */
static void test_sigkill_unmaskable(void) {
    puts("\n[SIGKILL unmaskable via rt_sigprocmask]\n");

    uint64_t mask = SIG_BIT(9) | SIG_BIT(19) | SIG_BIT(10); /* KILL|STOP|USR1 */
    long r = sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&mask, 0, 8);
    check_val("rt_sigprocmask(SIG_BLOCK, KILL|STOP|USR1)=0", r, 0);

    uint64_t got = ~0ULL;
    r = sc4(SYS_RT_SIGPROCMASK, 0, 0, (long)&got, 8);
    check_val("rt_sigprocmask(query)=0", r, 0);
    /* SIGKILL/SIGSTOP must not be in the returned blocked mask. */
    check("SIGKILL bit not in blocked mask", (got & SIG_BIT(9)) == 0);
    check("SIGSTOP bit not in blocked mask", (got & SIG_BIT(19)) == 0);
    /* SIGUSR1 should be there to prove the call took effect. */
    check("SIGUSR1 bit IS in blocked mask",  (got & SIG_BIT(10)) != 0);

    /* Cleanup: restore empty mask */
    uint64_t empty = 0;
    sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&empty, 0, 8);
}
TEST("signal/sigkill_unmaskable", test_sigkill_unmaskable);

/* External SIGKILL while child sleeps in nanosleep.
 * Child: long nanosleep. Parent: kill(child, SIGKILL); wait4.
 * Expect WIFSIGNALED && WTERMSIG=9 within ~500ms (i.e. wake-up was prompt,
 * not "wait for nanosleep deadline"). */
static void test_sigkill_in_nanosleep(void) {
    puts("\n[SIGKILL in nanosleep]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec ts = { 30, 0 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        sc1(SYS_EXIT_GROUP, 99); /* should not reach */
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);

    busy_wait_ms(50); /* let child enter the sleep */

    long r = sc2(SYS_KILL, pid, 9);
    check_val("kill(child, SIGKILL)=0", r, 0);

    int status = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("wait4 returns child pid", w, pid);
    check("WIFSIGNALED", WIFSIGNALED(status));
    check_val("WTERMSIG=SIGKILL", WTERMSIG(status), 9);
}
TEST("signal/sigkill_in_nanosleep", test_sigkill_in_nanosleep);

/* External SIGKILL while child blocks in pause() (rt_sigsuspend equivalent
 * via rt_sigsuspend with empty mask). Tests SIGKILL through sigsuspend's
 * own sleep loop. */
static void test_sigkill_in_sigsuspend(void) {
    puts("\n[SIGKILL in rt_sigsuspend]\n");

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Block all blockable signals, then sigsuspend with empty mask.
         * Standard pthread idiom; SIGKILL must still get through. */
        uint64_t full = ~SIG_BIT(9) & ~SIG_BIT(19);
        sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&full, 0, 8);

        uint64_t allow_kill = full; /* same mask: SIGKILL stays unblocked */
        sc2(SYS_RT_SIGSUSPEND, (long)&allow_kill, 8);

        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);

    busy_wait_ms(50);

    sc2(SYS_KILL, pid, 9);

    int status = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("wait4 returns child pid", w, pid);
    check("WIFSIGNALED", WIFSIGNALED(status));
    check_val("WTERMSIG=SIGKILL", WTERMSIG(status), 9);
}
TEST("signal/sigkill_in_sigsuspend", test_sigkill_in_sigsuspend);

/* External SIGKILL while child waits on a futex (private, value never
 * changes, infinite timeout). pthread_join uses exactly this pattern.
 * Without a working SIGKILL-through-futex_wait path, the parent's wait4
 * would block until QEMU times out the test runner. */
static void test_sigkill_in_futex(void) {
    puts("\n[SIGKILL in futex_wait]\n");

    /* Anonymous shared mapping is not strictly required here — the child's
     * futex sits at a private address that nobody will ever wake. The wake
     * must come from SIGKILL. We use a stack-local int for simplicity. */
    static volatile uint32_t futex_word = 0;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* FUTEX_WAIT_PRIVATE = 0|128, op=0 means WAIT, val=0 (matches),
         * timeout=NULL → wait forever. Only SIGKILL should release us. */
        sc6(SYS_FUTEX, (long)&futex_word, 0 | 128 /* WAIT|PRIVATE */, 0,
            0 /* no timeout */, 0, 0);
        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);

    busy_wait_ms(50);

    sc2(SYS_KILL, pid, 9);

    int status = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("wait4 returns child pid", w, pid);
    check("WIFSIGNALED", WIFSIGNALED(status));
    check_val("WTERMSIG=SIGKILL", WTERMSIG(status), 9);
}
TEST("signal/sigkill_in_futex", test_sigkill_in_futex);
