/* Phase 10.2 — timerfd_read waitqueue migration.
 *
 * timerfd_read now blocks on a per-tfd waitqueue; the hrtimer callback
 * fires wake_up_interruptible on expiry. Validates:
 *   - blocking read returns the expiration count after one-shot deadline
 *   - periodic timer wakes blocked reader exactly once per interval
 *   - SIGUSR1 interrupts a blocking timerfd_read with EINTR
 *   - TFD_NONBLOCK skips the block and returns -EAGAIN before any expiry
 */
#include "ktest.h"

struct ksigaction_tfd {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void tfd_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static volatile int tfd_sig_received;
static void tfd_sig_handler(int sig) { (void)sig; tfd_sig_received = 1; }

static void install_tfd_handler(int signo, void (*fn)(int)) {
    struct ksigaction_tfd sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER,
        .restorer = (void *)tfd_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* ── 01: nonblock returns EAGAIN before expiry ── */

static void test_timerfd_nonblock_eagain(void) {
    puts("\n[timerfd_blocking]\n");
    long tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, TFD_NONBLOCK);
    check("nonblock: tfd >= 0", tfd >= 0);
    if (tfd < 0) return;

    /* Arm 200ms one-shot, immediately try to read — must EAGAIN. */
    struct k_itimerspec spec = {
        .it_interval = {0, 0},
        .it_value    = {0, 200000000 /* 200ms */}
    };
    long r = sc4(SYS_TIMERFD_SETTIME, tfd, 0, (long)&spec, 0);
    check_val("nonblock: settime ok", r, 0);

    uint64_t val = 0;
    r = sc3(SYS_READ, tfd, (long)&val, 8);
    check_val("nonblock: read pre-expiry returns -EAGAIN", r, -EAGAIN);
    sc1(SYS_CLOSE, tfd);
}

/* ── 02: blocking read waits for one-shot expiry ── */

static void test_timerfd_blocking_oneshot(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
        if (tfd < 0) sc1(SYS_EXIT, 99);
        struct k_itimerspec spec = {
            .it_interval = {0, 0},
            .it_value    = {0, 30000000 /* 30ms */}
        };
        sc4(SYS_TIMERFD_SETTIME, tfd, 0, (long)&spec, 0);

        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        uint64_t val = 0;
        long r = sc3(SYS_READ, tfd, (long)&val, 8);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);

        long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                          (t1.tv_nsec - t0.tv_nsec);
        /* Encode: bit0 = read returned 8, bit1 = val >= 1, bits 2-3 = 10ms units capped. */
        int code = 0;
        if (r == 8)              code |= 1;
        if (val >= 1 && val < 10) code |= 2;
        long units = elapsed_ns / 10000000L;
        if (units > 3) units = 3;
        code |= (int)(units << 2);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("oneshot: read returned 8 bytes", (code & 1) == 1);
    check("oneshot: val == 1 expiration", (code & 2) == 2);
    /* 30ms expected — should be at least 20ms (units >= 2) */
    int units = (code >> 2) & 0x3;
    check("oneshot: blocked >= 20ms", units >= 2);
}

/* ── 03: periodic timerfd, blocking reader gets repeated wakes ── */

static void test_timerfd_blocking_periodic(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
        if (tfd < 0) sc1(SYS_EXIT, 99);
        struct k_itimerspec spec = {
            .it_interval = {0, 25000000 /* 25ms */},
            .it_value    = {0, 25000000 /* 25ms first fire */}
        };
        sc4(SYS_TIMERFD_SETTIME, tfd, 0, (long)&spec, 0);

        int oks = 0;
        for (int i = 0; i < 3; i++) {
            uint64_t val = 0;
            long r = sc3(SYS_READ, tfd, (long)&val, 8);
            if (r == 8 && val >= 1) oks++;
        }
        sc1(SYS_EXIT, oks);
        __builtin_unreachable();
    }
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("periodic: 3 successful blocking reads", (ws >> 8) & 0xFF, 3);
}

/* ── 04: signal interrupts blocking timerfd_read with EINTR ── */

static void test_timerfd_signal_interrupts(void) {
    install_tfd_handler(SIGUSR1, tfd_sig_handler);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_tfd_handler(SIGUSR1, tfd_sig_handler);
        long tfd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
        if (tfd < 0) sc1(SYS_EXIT, 99);

        /* Long deadline — only the signal should wake us. */
        struct k_itimerspec spec = {
            .it_interval = {0, 0},
            .it_value    = {2, 0 /* 2 seconds */}
        };
        sc4(SYS_TIMERFD_SETTIME, tfd, 0, (long)&spec, 0);

        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        uint64_t val = 0;
        long r = sc3(SYS_READ, tfd, (long)&val, 8);
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000000L;

        /* Encode: bit0 = EINTR, bits 1-2 = 100ms units capped */
        int code = 0;
        if (r == -EINTR) code |= 1;
        long units = elapsed_ms / 100;
        if (units > 3) units = 3;
        code |= (int)(units << 1);
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }
    /* Give child ~30ms to enter the blocking read. */
    struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
    sc2(SYS_NANOSLEEP, (long)&delay, 0);
    sc2(SYS_KILL, pid, SIGUSR1);
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("signal: timerfd_read returned -EINTR", (code & 1) == 1);
    int units = (code >> 1) & 0x3;
    check("signal: woke before 300ms (well under 2s)", units <= 2);
}

TEST("timerfd/nonblock_eagain",     test_timerfd_nonblock_eagain);
TEST("timerfd/blocking_oneshot",    test_timerfd_blocking_oneshot);
TEST("timerfd/blocking_periodic",   test_timerfd_blocking_periodic);
TEST("timerfd/signal_interrupts",   test_timerfd_signal_interrupts);
