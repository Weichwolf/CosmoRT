/* Linux prepare_to_wait/finish_wait adaption: missed-wakeup-race guard.
 *
 * Scenario (LTP clock_nanosleep01 SEND_SIGINT subcase):
 *   - Parent calls clock_nanosleep(10s).
 *   - Child kills(parent, SIGINT) after a short delay.
 *   - Signal arrives while parent is between first pending-check and
 *     state=THREAD_BLOCKED in thread_block_ms. Without the wakeup_pending
 *     flag the sched_wake CAS fires on state=RUNNING, fails, and the parent
 *     sleeps for the full 10s.
 *
 * Guarantees after the fix:
 *   - thread_block_ms returns within a few ms of signal arrival.
 *   - clock_nanosleep exits with EINTR.
 */

#include "ktest.h"

#define SIGINT 2

struct ksigaction {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void sr_restorer(void) {
    __asm__ volatile("mov $15, %%rax\n\tsyscall\n" ::: "memory");
}

static volatile int sigint_count = 0;
__attribute__((used)) static void sr_handler(int sig) {
    (void)sig;
    sigint_count++;
}

static void test_signal_sleep_race(void) {
    puts("\n[signal_sleep_race]\n");

    /* Install a real handler so delivery is synchronous via check_pending_signals. */
    struct ksigaction sa = {
        .handler  = (void *)sr_handler,
        .flags    = SA_RESTORER,
        .restorer = (void *)sr_restorer,
        .mask     = 0,
    };
    long r = sc4(SYS_RT_SIGACTION, SIGINT, (long)&sa, 0, 8);
    check_val("rt_sigaction(SIGINT)", r, 0);

    long ppid = sc0(SYS_GETPID);

    /* Child: sleep 100ms, then send SIGINT to parent. */
    long kid = sc0(SYS_FORK);
    if (kid == 0) {
        struct k_timespec wait = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        sc2(SYS_NANOSLEEP, (long)&wait, 0);
        sc2(SYS_KILL, ppid, SIGINT);
        sc1(SYS_EXIT, 0);
    }
    check("fork ok", kid > 0);

    /* Parent: sleep 10s. Should return EINTR in < 300ms after signal. */
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);

    struct k_timespec long_sleep = { .tv_sec = 10, .tv_nsec = 0 };
    struct k_timespec remainder = { 0, 0 };
    long rc = sc2(SYS_NANOSLEEP, (long)&long_sleep, (long)&remainder);

    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);

    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
                    + (after.tv_nsec - before.tv_nsec) / 1000000;

    check_val("nanosleep interrupted with EINTR", rc, -EINTR);
    check("nanosleep returned in < 1000ms (signal-wake, no stale timer)",
          elapsed_ms < 1000);
    check("nanosleep handler ran", sigint_count >= 1);
    check("remainder left > 8s",
          remainder.tv_sec >= 8);

    int status = 0;
    sc4(SYS_WAIT4, kid, (long)&status, 0, 0);
    check("child exited cleanly", (status & 0x7F) == 0);
}

TEST("signal-sleep-race", test_signal_sleep_race);
