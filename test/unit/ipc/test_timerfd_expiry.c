#include "ktest.h"

/* ── Timerfd expiry: TSC-based interrupt-driven wakeup ── */

struct tfd_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

#define T_EPOLLIN      0x001
#define T_EPOLL_CTL_ADD 1

/* Helper: create armed timerfd with given ms values */
static long tfd_create_armed(uint64_t val_ms, uint64_t int_ms) {
    long fd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
    if (fd < 0) return fd;
    struct k_itimerspec its;
    its.it_value.tv_sec = (long)(val_ms / 1000);
    its.it_value.tv_nsec = (long)((val_ms % 1000) * 1000000);
    its.it_interval.tv_sec = (long)(int_ms / 1000);
    its.it_interval.tv_nsec = (long)((int_ms % 1000) * 1000000);
    long r = sc4(SYS_TIMERFD_SETTIME, fd, 0, (long)&its, 0);
    if (r < 0) { sc1(SYS_CLOSE, fd); return r; }
    return fd;
}

static void test_timerfd_create_basic(void) {
    puts("\n[timerfd_expiry]\n");
    long fd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0);
    check("timerfd_create >= 0", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);
}

static void test_timerfd_arm_and_read(void) {
    long fd = tfd_create_armed(10, 0); /* 10ms one-shot */
    check("arm 10ms", fd >= 0);
    if (fd < 0) return;
    /* Sleep 30ms to ensure expiry */
    struct k_timespec ts = { 0, 30000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    uint64_t val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("read returns 8", r, 8);
    check("expirations >= 1", val >= 1);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_poll_ready(void) {
    long fd = tfd_create_armed(5, 0); /* 5ms one-shot */
    check("arm 5ms", fd >= 0);
    if (fd < 0) return;
    /* Wait 20ms */
    struct k_timespec ts = { 0, 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    /* poll should show POLLIN */
    struct { int fd; short events; short revents; } pfd;
    pfd.fd = (int)fd;
    pfd.events = 0x0001; /* POLLIN */
    pfd.revents = 0;
    long r = sc3(SYS_POLL, (long)&pfd, 1, 0);
    check("poll shows ready", r >= 1);
    check("POLLIN set", (pfd.revents & 0x0001) != 0);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_epoll_ready(void) {
    long fd = tfd_create_armed(5, 0); /* 5ms one-shot */
    check("arm 5ms epoll", fd >= 0);
    if (fd < 0) return;
    long epfd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_CLOSE, fd); return; }
    struct tfd_epoll_event ev = { T_EPOLLIN, 99 };
    sc4(SYS_EPOLL_CTL, epfd, T_EPOLL_CTL_ADD, fd, (long)&ev);
    /* Wait via epoll_wait with 100ms timeout — should wake on timerfd expiry */
    struct tfd_epoll_event out;
    long r = sc4(SYS_EPOLL_WAIT, epfd, (long)&out, 1, 100);
    check("epoll_wait returns 1", r == 1);
    if (r == 1) check_val("epoll data", (long)out.data, 99);
    sc1(SYS_CLOSE, epfd);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_not_ready_before_expiry(void) {
    long fd = tfd_create_armed(500, 0); /* 500ms — won't expire in time */
    check("arm 500ms", fd >= 0);
    if (fd < 0) return;
    /* poll with 0 timeout — should not be ready */
    struct { int fd; short events; short revents; } pfd;
    pfd.fd = (int)fd;
    pfd.events = 0x0001;
    pfd.revents = 0;
    long r = sc3(SYS_POLL, (long)&pfd, 1, 0);
    check_val("poll 0ms not ready", r, 0);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_interval(void) {
    long fd = tfd_create_armed(5, 5); /* 5ms initial, 5ms interval */
    check("arm interval", fd >= 0);
    if (fd < 0) return;
    /* Sleep 30ms — should accumulate multiple expirations */
    struct k_timespec ts = { 0, 30000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    uint64_t val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("interval read 8", r, 8);
    check("interval expirations >= 2", val >= 2);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_read_resets(void) {
    long fd = tfd_create_armed(5, 0); /* 5ms one-shot */
    check("arm reset", fd >= 0);
    if (fd < 0) return;
    struct k_timespec ts = { 0, 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    uint64_t val = 0;
    sc3(SYS_READ, fd, (long)&val, 8);
    check("first read > 0", val > 0);
    /* Second read should return EAGAIN (no new expirations, one-shot disarmed) */
    val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("second read EAGAIN", r, -EAGAIN);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_disarm(void) {
    long fd = tfd_create_armed(5, 5); /* armed */
    check("arm for disarm", fd >= 0);
    if (fd < 0) return;
    /* Disarm: set to 0 */
    struct k_itimerspec its;
    its.it_value.tv_sec = 0; its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 0;
    long r = sc4(SYS_TIMERFD_SETTIME, fd, 0, (long)&its, 0);
    check_val("disarm", r, 0);
    /* Sleep and verify no expirations */
    struct k_timespec ts = { 0, 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    struct { int fd; short events; short revents; } pfd;
    pfd.fd = (int)fd;
    pfd.events = 0x0001;
    pfd.revents = 0;
    r = sc3(SYS_POLL, (long)&pfd, 1, 0);
    check_val("disarmed not ready", r, 0);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_zero_timeout_read(void) {
    /* Create timerfd with TFD_NONBLOCK, read without arming → EAGAIN */
    long fd = sc2(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, (long)TFD_NONBLOCK);
    check("create nonblock", fd >= 0);
    if (fd < 0) return;
    uint64_t val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("unarmed read EAGAIN", r, -EAGAIN);
    sc1(SYS_CLOSE, fd);
}

static void test_timerfd_back_to_back(void) {
    /* Arm, read, re-arm, read — verifies re-arming works */
    long fd = tfd_create_armed(5, 0);
    check("arm b2b", fd >= 0);
    if (fd < 0) return;
    struct k_timespec ts = { 0, 20000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    uint64_t val = 0;
    sc3(SYS_READ, fd, (long)&val, 8);
    check("b2b first read", val > 0);
    /* Re-arm */
    struct k_itimerspec its;
    its.it_value.tv_sec = 0; its.it_value.tv_nsec = 5000000; /* 5ms */
    its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 0;
    sc4(SYS_TIMERFD_SETTIME, fd, 0, (long)&its, 0);
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    val = 0;
    long r = sc3(SYS_READ, fd, (long)&val, 8);
    check_val("b2b second read 8", r, 8);
    check("b2b second val > 0", val > 0);
    sc1(SYS_CLOSE, fd);
}

TEST("timerfd_expiry", test_timerfd_create_basic);
TEST("timerfd_arm_read", test_timerfd_arm_and_read);
TEST("timerfd_poll_ready", test_timerfd_poll_ready);
TEST("timerfd_epoll_ready", test_timerfd_epoll_ready);
TEST("timerfd_not_ready", test_timerfd_not_ready_before_expiry);
TEST("timerfd_interval", test_timerfd_interval);
TEST("timerfd_read_reset", test_timerfd_read_resets);
TEST("timerfd_disarm", test_timerfd_disarm);
TEST("timerfd_zero_read", test_timerfd_zero_timeout_read);
TEST("timerfd_back2back", test_timerfd_back_to_back);
