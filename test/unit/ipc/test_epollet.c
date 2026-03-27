#include "ktest.h"

/* ── EPOLLET ─────────────────────────────────── */

#define T_EPOLLIN  0x001
#define T_EPOLLET  (1U << 31)
#define T_EPOLL_CTL_ADD 1

struct test_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

static void test_epollet(void) {
    puts("\n[EPOLLET]\n");

    long efd = sc2(SYS_EVENTFD2, 0, 0);
    check("eventfd2", efd >= 0);
    if (efd < 0) return;

    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, efd); return; }

    struct test_epoll_event ev;
    ev.events = T_EPOLLIN | T_EPOLLET;
    ev.data = 42;
    long r = sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, efd, (long)&ev);
    check_val("epoll_ctl ADD ET", r, 0);

    /* Write to eventfd */
    uint64_t val = 1;
    sc3(SYS_WRITE, efd, (long)&val, 8);

    /* First wait: should see event */
    struct test_epoll_event out;
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0);
    check_val("epoll_wait ET first", r, 1);

    /* Second wait: edge consumed, should return 0 */
    r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 0);
    check_val("epoll_wait ET second = 0", r, 0);

    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, efd);
}

TEST("EPOLLET", test_epollet);
