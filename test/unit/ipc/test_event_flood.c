/* Class A: Event-Queue Ring-Growth.
 *
 * Viele pending Socket-Data-Events → Ring waechst (Initial 256, verdoppelt).
 * Strategie: 300 pipes, schreibe in alle. epoll_wait auf alle fds. */
#include "ktest.h"

#define PIPE_COUNT      300
#define EPOLL_BATCH     320

#define EPOLLIN         0x001

struct kevent {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#define EPOLL_CTL_ADD   1

static void test_event_flood_epoll(void) {
    puts("\n[EVENT_FLOOD_EPOLL]\n");

    long ep = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", ep >= 0);
    if (ep < 0) return;

    long lim_cur = 4096, lim_max = 65536;
    long r;
    struct { unsigned long cur, max; } lim = { 4096, 65536 };
    (void)lim_cur; (void)lim_max;
    r = sc4(SYS_PRLIMIT64, 0, 7 /* RLIMIT_NOFILE */, (long)&lim, 0);
    (void)r;

    int fds[PIPE_COUNT][2];
    int opened = 0;
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (sc2(SYS_PIPE2, (long)fds[i], 0) < 0) break;
        opened++;
    }
    check_ge("opened >256 pipes", opened, 257);

    int added = 0;
    for (int i = 0; i < opened; i++) {
        struct kevent ev = { EPOLLIN, (uint64_t)i };
        if (sc4(SYS_EPOLL_CTL, ep, EPOLL_CTL_ADD, fds[i][0], (long)&ev) == 0)
            added++;
    }
    check_val("epoll_ctl ADD all", added, opened);

    for (int i = 0; i < opened; i++)
        sc3(SYS_WRITE, fds[i][1], (long)"x", 1);

    struct kevent out[EPOLL_BATCH];
    long n = sc4(SYS_EPOLL_WAIT, ep, (long)out, EPOLL_BATCH, 1000);
    check_ge("epoll reports >256 ready", n, 257);

    char buf;
    for (int i = 0; i < opened; i++)
        sc3(SYS_READ, fds[i][0], (long)&buf, 1);

    for (int i = 0; i < opened; i++) {
        sc1(SYS_CLOSE, fds[i][0]);
        sc1(SYS_CLOSE, fds[i][1]);
    }
    sc1(SYS_CLOSE, ep);
}

TEST("event/flood_epoll", test_event_flood_epoll);
