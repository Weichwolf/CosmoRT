/* CosmoRT wait4 children_wq tests
 *
 * Phase 10.2b-7: wait4 blocks on parent->children_wq instead of per-thread eq.
 * Cover the four shapes that exercise the new wake/race path:
 *   - block_wakeup_on_exit: parent enters wait4, child exits, parent unblocks
 *     with the correct WIFEXITED status.
 *   - block_signal_eintr: parent in wait4, unrelated SIGUSR1 (no SA_RESTART)
 *     converts -ERESTARTSYS to -EINTR at the syscall return.
 *   - wnohang_returns_zero: WNOHANG with a still-running child returns 0.
 *   - multiple_children: parent waits for 3 children, all reaped in some order.
 */
#include "ktest.h"
#include "linux/signal.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

#ifndef WNOHANG
#define WNOHANG 1
#endif

__attribute__((naked)) static void wait4_sig_restorer(void) {
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}

static volatile int wait4_sig_seen;
static void wait4_sig_handler(int sig) { (void)sig; wait4_sig_seen = 1; }

static void install_eintr_handler(int signo) {
    /* No SA_RESTART: SIGUSR1 must convert -ERESTARTSYS to -EINTR. */
    struct k_sigaction sa = { 0 };
    sa.sa_handler = (void *)wait4_sig_handler;
    sa.sa_flags   = SA_RESTORER;
    sa.sa_restorer = (void *)wait4_sig_restorer;
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* ── 01 block_wakeup_on_exit ───────────────────── */

static void test_wait4_block_wakeup_on_exit(void) {
    puts("\n[wait4/block_wakeup_on_exit]\n");
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Sleep ~50ms so parent enters wait4 first, then exit(7). */
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        sc1(SYS_EXIT_GROUP, 7);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);
    if (pid <= 0) return;

    int wstatus = 0;
    long r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child pid", r, pid);
    check("WIFEXITED", WIFEXITED(wstatus));
    check_val("WEXITSTATUS == 7", WEXITSTATUS(wstatus), 7);
}
TEST("wait4/block_wakeup_on_exit", test_wait4_block_wakeup_on_exit);

/* ── 02 block_signal_eintr ─────────────────────── */

static void test_wait4_block_signal_eintr(void) {
    puts("\n[wait4/block_signal_eintr]\n");
    install_eintr_handler(SIGUSR1);
    wait4_sig_seen = 0;

    long parent_pid = sc0(SYS_GETPID);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: small delay, then signal parent and exit much later. */
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 30000000 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        sc2(SYS_KILL, parent_pid, SIGUSR1);
        struct k_timespec ts2 = { .tv_sec = 5, .tv_nsec = 0 };
        sc2(SYS_NANOSLEEP, (long)&ts2, 0);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);
    if (pid <= 0) return;

    /* Parent blocks in wait4. SIGUSR1 (no SA_RESTART) interrupts → -EINTR. */
    int wstatus = 0;
    long r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 -> -EINTR", r, -EINTR);
    check("SIGUSR1 handler ran", wait4_sig_seen != 0);

    /* Reap the child so we don't leak a zombie into later tests. */
    sc2(SYS_KILL, pid, SIGTERM);
    int ws2 = 0;
    sc4(SYS_WAIT4, pid, (long)&ws2, 0, 0);
}
TEST("wait4/block_signal_eintr", test_wait4_block_signal_eintr);

/* ── 03 wnohang_returns_zero ───────────────────── */

static void test_wait4_wnohang_returns_zero(void) {
    puts("\n[wait4/wnohang_returns_zero]\n");
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child: alive long enough that WNOHANG-poll sees no status. */
        struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);
    if (pid <= 0) return;

    int wstatus = 0;
    long r = sc4(SYS_WAIT4, pid, (long)&wstatus, WNOHANG, 0);
    check_val("WNOHANG with running child -> 0", r, 0);

    /* Now block-reap. */
    long r2 = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("blocking wait4 reaps", r2, pid);
    check("WIFEXITED", WIFEXITED(wstatus));
}
TEST("wait4/wnohang_returns_zero", test_wait4_wnohang_returns_zero);

/* ── 04 multiple_children ──────────────────────── */

static void test_wait4_multiple_children(void) {
    puts("\n[wait4/multiple_children]\n");
    long pids[3];
    int forked = 0;
    for (int i = 0; i < 3; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            /* Each child sleeps a different short interval, then exits with i. */
            struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 + (long)i * 15000000 };
            sc2(SYS_NANOSLEEP, (long)&ts, 0);
            sc1(SYS_EXIT_GROUP, 100 + i);
            __builtin_unreachable();
        }
        if (pid <= 0) break;
        pids[forked++] = pid;
    }
    check_val("forked 3 children", (long)forked, 3);
    if (forked != 3) return;

    int reaped = 0;
    int seen[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        int wstatus = 0;
        long r = sc4(SYS_WAIT4, -1, (long)&wstatus, 0, 0);
        if (r <= 0) continue;
        if (!WIFEXITED(wstatus)) continue;
        int code = WEXITSTATUS(wstatus);
        if (code >= 100 && code <= 102) {
            seen[code - 100] = 1;
            reaped++;
        }
    }
    check_val("all 3 children reaped", (long)reaped, 3);
    check("child 0 reaped", seen[0]);
    check("child 1 reaped", seen[1]);
    check("child 2 reaped", seen[2]);

    /* No more children → -ECHILD. */
    int ws = 0;
    long ech = sc4(SYS_WAIT4, -1, (long)&ws, 0, 0);
    check_val("wait4 -> -ECHILD when no children", ech, -ECHILD);
}
TEST("wait4/multiple_children", test_wait4_multiple_children);
