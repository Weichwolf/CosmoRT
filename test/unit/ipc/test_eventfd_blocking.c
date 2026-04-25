/* Phase 10.2 — eventfd_read waitqueue migration.
 *
 * eventfd_read now blocks on a per-eventfd waitqueue; eventfd_write fires
 * wake_up_interruptible after the counter goes non-zero. Validates:
 *   - blocking read wakes when a writer in another process bumps the counter
 *   - SIGUSR1 interrupts a blocking eventfd_read with EINTR
 *   - EFD_NONBLOCK skips the block and returns -EAGAIN on counter==0
 */
#include "ktest.h"

struct ksigaction_efd {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void efd_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static void efd_sig_handler(int sig) { (void)sig; }

static void install_efd_handler(int signo, void (*fn)(int)) {
    struct ksigaction_efd sa = {
        .handler = (void *)fn,
        .flags = SA_RESTORER,
        .restorer = (void *)efd_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, signo, (long)&sa, 0, 8);
}

/* ── 01: nonblock returns EAGAIN when counter==0 ── */

static void test_eventfd_nonblock_eagain(void) {
    puts("\n[eventfd_blocking]\n");
    long efd = sc2(SYS_EVENTFD2, 0, (long)EFD_NONBLOCK);
    check("nonblock: efd >= 0", efd >= 0);
    if (efd < 0) return;

    uint64_t val = 0;
    long r = sc3(SYS_READ, efd, (long)&val, 8);
    check_val("nonblock: read empty returns -EAGAIN", r, -EAGAIN);
    sc1(SYS_CLOSE, efd);
}

/* ── 02: blocking read wakes when forked writer bumps counter ──
 * Parent creates eventfd, forks. Child sleeps 30ms then writes 0x42.
 * Parent does blocking read — expects val==0x42 and elapsed >= 20ms,
 * proving it actually slept rather than spun. */

static void test_eventfd_blocking_wake(void) {
    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("wake: efd >= 0", efd >= 0);
    if (efd < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 /* 30ms */ };
        sc2(SYS_NANOSLEEP, (long)&delay, 0);
        uint64_t v = 0x42;
        sc3(SYS_WRITE, efd, (long)&v, 8);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }

    struct k_timespec t0, t1;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
    uint64_t val = 0;
    long r = sc3(SYS_READ, efd, (long)&val, 8);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000L;

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    check_val("wake: read returned 8 bytes",     r, 8);
    check_val("wake: counter value matches write", (long)val, 0x42);
    check("wake: blocked >= 20ms (real sleep)",  elapsed_ms >= 20);
    sc1(SYS_CLOSE, efd);
}

/* ── 03: signal interrupts blocking eventfd_read with EINTR ──
 * Child blocks on empty eventfd; parent SIGUSR1s after 30ms. Expect EINTR
 * before 500ms (well under any meaningful spin/timeout). */

static void test_eventfd_signal_interrupts(void) {
    install_efd_handler(SIGUSR1, efd_sig_handler);
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        install_efd_handler(SIGUSR1, efd_sig_handler);
        long efd = sc2(SYS_EVENTFD2, 0, 0);
        if (efd < 0) sc1(SYS_EXIT, 99);

        struct k_timespec t0, t1;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
        uint64_t val = 0;
        long r = sc3(SYS_READ, efd, (long)&val, 8);
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
    check("signal: eventfd_read returned -EINTR", (code & 1) == 1);
    int units = (code >> 1) & 0x3;
    check("signal: woke before 300ms",            units <= 2);
}

TEST("eventfd/nonblock_eagain",     test_eventfd_nonblock_eagain);
TEST("eventfd/blocking_wake",       test_eventfd_blocking_wake);
TEST("eventfd/signal_interrupts",   test_eventfd_signal_interrupts);
