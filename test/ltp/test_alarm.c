/* LTP alarm02/alarm03/alarm05/alarm06/alarm07 — ported to ktest */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

/*
 * Signal restorer trampoline — required by rt_sigaction on x86_64.
 * The kernel expects sa_restorer to point at code executing SYS_RT_SIGRETURN.
 */
__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}

/* Install a signal handler for SIGALRM using raw rt_sigaction */
static volatile int alarms_received;

static void sigalrm_handler(int sig) {
    if (sig == SIGALRM)
        alarms_received++;
}

static void install_sigalrm(void) {
    struct k_sigaction sa;
    sa.sa_handler = (void *)sigalrm_handler;
    sa.sa_flags = SA_RESTORER;
    sa.sa_restorer = (void *)sig_restorer;
    sa.sa_mask = 0;
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);
}

/* ── alarm02: alarm(0) cancels pending, returns remaining ── */

static void test_alarm02(void) {
    puts("\n[ltp/alarm02]\n");
    install_sigalrm();
    alarms_received = 0;

    sc1(SYS_ALARM, 0); /* cancel test-runner watchdog */
    long r = sc1(SYS_ALARM, 100);
    check_val("alarm(100) first call returns 0", r, 0);

    /* Cancel it — should return ~100 (allow some slack) */
    r = sc1(SYS_ALARM, 0);
    check("alarm(0) returns remaining ~100", r >= 99 && r <= 100);

    /* No SIGALRM should have been delivered */
    check_val("no SIGALRM", alarms_received, 0);
}

/* ── alarm03: alarm replaces previous, returns remaining ── */

static void test_alarm03(void) {
    puts("\n[ltp/alarm03]\n");
    install_sigalrm();
    alarms_received = 0;

    sc1(SYS_ALARM, 0); /* cancel test-runner watchdog */
    long r = sc1(SYS_ALARM, 100);
    check_val("alarm(100) returns 0", r, 0);

    /* Replace with shorter alarm — should return ~100 */
    r = sc1(SYS_ALARM, 50);
    check("alarm(50) returns ~100", r >= 99 && r <= 100);

    /* Cancel */
    r = sc1(SYS_ALARM, 0);
    check("alarm(0) returns ~50", r >= 49 && r <= 50);
}

/* ── alarm05: alarm fires SIGALRM after specified time ── */

static void test_alarm05(void) {
    puts("\n[ltp/alarm05]\n");
    install_sigalrm();
    alarms_received = 0;

    sc1(SYS_ALARM, 0); /* cancel test-runner watchdog */
    long r = sc1(SYS_ALARM, 1);
    check_val("alarm(1) returns 0", r, 0);

    /* Sleep 2 seconds to let it fire */
    struct k_timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    check_val("SIGALRM received", alarms_received, 1);
}

/* ── alarm06: alarm(0) cancels pending — no signal after sleep ── */

static void test_alarm06(void) {
    puts("\n[ltp/alarm06]\n");
    install_sigalrm();
    alarms_received = 0;

    /* Set alarm for 2 seconds */
    sc1(SYS_ALARM, 2);

    /* Sleep 1 second */
    struct k_timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Cancel — should return ~1 */
    long r = sc1(SYS_ALARM, 0);
    check("alarm(0) returns ~1", r >= 1 && r <= 2);

    /* Sleep past when alarm would have fired */
    ts.tv_sec = 2;
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    check_val("no SIGALRM after cancel", alarms_received, 0);
}

/* ── alarm07: alarm not inherited by child ── */

static void test_alarm07(void) {
    puts("\n[ltp/alarm07]\n");
    install_sigalrm();

    /* Set alarm in parent */
    sc1(SYS_ALARM, 100);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        /* Child: alarm(0) should return 0 — alarm not inherited */
        long r = sc1(SYS_ALARM, 0);
        sc1(SYS_EXIT_GROUP, (r == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    /* Parent: alarm(0) should return ~100 */
    long r = sc1(SYS_ALARM, 0);
    check("parent alarm(0) returns ~100", r >= 99 && r <= 100);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child alarm(0)==0", (long)WEXITSTATUS(wstatus), 0);
}

/* ── alarm07b: SIGALRM not delivered to child ── */

static void test_alarm07b(void) {
    puts("\n[ltp/alarm07b]\n");
    install_sigalrm();
    alarms_received = 0;

    sc1(SYS_ALARM, 1);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        /* Child: reset counter, brief sleep, check no alarm */
        alarms_received = 0;
        struct k_timespec ts;
        ts.tv_sec = 2;
        ts.tv_nsec = 0;
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        sc1(SYS_EXIT_GROUP, (alarms_received == 0) ? 0 : 1);
        __builtin_unreachable();
    }

    /* Parent: sleep 2s — alarm(1) should interrupt after 1s */
    struct k_timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    check_val("parent got SIGALRM", alarms_received, 1);

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));
    check_val("child got 0 alarms", (long)WEXITSTATUS(wstatus), 0);
}

TEST("ltp/alarm02",  test_alarm02);
TEST("ltp/alarm03",  test_alarm03);
TEST("ltp/alarm05",  test_alarm05);
TEST("ltp/alarm06",  test_alarm06);
TEST("ltp/alarm07",  test_alarm07);
TEST("ltp/alarm07b", test_alarm07b);
