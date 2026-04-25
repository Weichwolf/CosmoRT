/* CosmoRT event_wait race-regression ktests (Phase 10.2c)
 *
 * event_wait_ns ist auf prepare_to_wait/finish_wait migriert. Der alte
 * naked state=BLOCKED+mfence+schedule()-Pfad hatte unter tickless LAPIC
 * deterministisch Hangs in musl sem_init/tls_init produziert (Race
 * zwischen Ring-Empty-Check und schedule() ohne wq-lock-Serialisierung).
 *
 * Diese Tests reproduzieren das Race-Window auf event_wait-getragenen
 * Pfaden:
 *   - eventfd_read blockiert via event_wait, eventfd_write event_postet
 *   - epoll_wait blockiert via event_wait_ns, fd-Ready event_postet
 *   - wait4 blockiert via event_wait, SIGCHLD-Exit event_postet
 *   - Konkurrent posten + warten auf grossen Iteration-Counts
 *
 * Wenn das Race nicht geschlossen ist hangt einer dieser Tests
 * deterministisch unter -smp 2 + tickless LAPIC.
 */

#include "ktest.h"

/* ── 01: eventfd read+write race, 100 Iterationen ─── */

static void test_evw_01_eventfd_pingpong(void) {
    puts("\n[event_wait race]\n");

    int efd = (int)sc2(SYS_EVENTFD2, 0, 0);
    if (efd < 0) { fail("eventfd2", 0); return; }

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        for (int i = 0; i < 100; i++) {
            uint64_t v = 1;
            sc3(SYS_WRITE, efd, (long)&v, 8);
        }
        sc1(SYS_CLOSE, efd);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    if (pid < 0) { fail("fork", 0); return; }

    int reads = 0;
    uint64_t total = 0;
    while (total < 100) {
        uint64_t v = 0;
        long r = sc3(SYS_READ, efd, (long)&v, 8);
        if (r != 8) break;
        reads++;
        total += v;
    }
    sc1(SYS_CLOSE, efd);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);

    check_val("eventfd 100x ping-pong total", (long)total, 100);
    check_ge("eventfd reads >= 1", reads, 1);
}

/* ── 02: eventfd race-window mit kurzen Intervallen ─ */

static void test_evw_02_eventfd_tight_race(void) {
    /* Producer postet sofort nach fork, Consumer ist u.U. noch nicht in
     * event_wait. Wiederholen unter tight loop deckt das Race auf, das
     * pre-waitqueue zwischen ring-check und schedule() lebte. */
    int efd = (int)sc2(SYS_EVENTFD2, 0, 0);
    if (efd < 0) { fail("eventfd2", 0); return; }

    int rounds = 50;
    int ok = 0;
    for (int i = 0; i < rounds; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            uint64_t v = 1;
            /* Sofort posten — Parent ist noch nicht zwingend im read */
            sc3(SYS_WRITE, efd, (long)&v, 8);
            sc1(SYS_EXIT, 0);
            __builtin_unreachable();
        }
        uint64_t v = 0;
        long r = sc3(SYS_READ, efd, (long)&v, 8);
        int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
        if (r == 8 && v == 1) ok++;
    }
    sc1(SYS_CLOSE, efd);
    check_val("50 tight eventfd races all completed", ok, rounds);
}

/* ── 03: epoll_wait race mit eventfd ──────────────── */

static void test_evw_03_epoll_eventfd(void) {
    int efd = (int)sc2(SYS_EVENTFD2, 0, 0);
    if (efd < 0) { fail("eventfd2", 0); return; }
    int ep = (int)sc1(SYS_EPOLL_CREATE1, 0);
    if (ep < 0) { sc1(SYS_CLOSE, efd); fail("epoll_create1", 0); return; }

    struct k_epoll_event { uint32_t events; uint64_t data; } __attribute__((packed));
    struct k_epoll_event ev = { .events = 1 /* EPOLLIN */, .data = 42 };
    sc4(SYS_EPOLL_CTL, ep, 1 /* ADD */, efd, (long)&ev);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        struct k_timespec d = { .tv_sec = 0, .tv_nsec = 5000000 };
        sc2(SYS_NANOSLEEP, (long)&d, 0);
        uint64_t v = 7;
        sc3(SYS_WRITE, efd, (long)&v, 8);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }

    struct k_epoll_event out[4];
    long n = sc4(SYS_EPOLL_WAIT, ep, (long)out, 4, 1000 /* 1s timeout */);
    int ws = 0; sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    sc1(SYS_CLOSE, ep); sc1(SYS_CLOSE, efd);

    check_val("epoll_wait returns 1 fd", n, 1);
    check_val("epoll fd has data sentinel", (long)out[0].data, 42);
}

/* ── 04: epoll_wait timeout precision (event_wait_ns hrtimer-Pfad) ─ */

static void test_evw_04_epoll_timeout(void) {
    int ep = (int)sc1(SYS_EPOLL_CREATE1, 0);
    if (ep < 0) { fail("epoll_create1", 0); return; }

    struct k_timespec t0, t1;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
    struct k_epoll_event { uint32_t events; uint64_t data; } __attribute__((packed));
    struct k_epoll_event out[4];
    long n = sc4(SYS_EPOLL_WAIT, ep, (long)out, 4, 50 /* 50ms */);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    sc1(SYS_CLOSE, ep);

    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check_val("empty epoll timeout returns 0", n, 0);
    check("epoll 50ms timeout >= 40ms", elapsed_ms >= 40);
    check("epoll 50ms timeout < 500ms (no missed wake)", elapsed_ms < 500);
}

/* ── 05: wait4 race — child exit triggers event_post, parent in event_wait ─ */

static void test_evw_05_wait4_burst(void) {
    /* 30 short-lived children, parent reaps each. wait4 internally calls
     * event_wait. Child exit calls event_post(EQ_CHILD_EXITED). The race
     * pre-waitqueue: child exited between parent's empty-ring check and
     * its schedule() — wakeup lost, parent slept. With prepare_to_wait
     * the wq-lock serialization makes that impossible. */
    int reaped = 0;
    for (int i = 0; i < 30; i++) {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            sc1(SYS_EXIT, i & 0xFF);
            __builtin_unreachable();
        }
        if (pid < 0) break;
        int ws = 0;
        long r = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
        if (r == pid) reaped++;
    }
    check_val("30x fork+wait4 race reaped all", reaped, 30);
}

TEST("event_wait_race/01_eventfd_pingpong", test_evw_01_eventfd_pingpong);
TEST("event_wait_race/02_eventfd_tight",    test_evw_02_eventfd_tight_race);
TEST("event_wait_race/03_epoll_eventfd",    test_evw_03_epoll_eventfd);
TEST("event_wait_race/04_epoll_timeout",    test_evw_04_epoll_timeout);
TEST("event_wait_race/05_wait4_burst",      test_evw_05_wait4_burst);
