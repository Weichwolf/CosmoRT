/* IPC wait_any: multi-endpoint event_wait blocking tests
 *
 * IPC is kernel-internal (no syscall). These tests validate the
 * multi-source wake pattern that ipc_wait_any now uses, via:
 * - EQ_IPC_MSG event queue with multiple sources
 * - poll/epoll multi-fd wake (same event_wait-based blocking)
 * - Thread wake from any-of-N sources correctness
 */
#include "ktest.h"
#include "core/event_queue.h"

#define THREAD_STACK 65536

struct test_pollfd { int fd; short events; short revents; };
#define T_POLLIN  0x0001

static volatile int any_worker_done;
static volatile long any_worker_result;
static volatile int any_worker_started;

static long spawn_thread(void (*fn)(void)) {
    long stk = sc6(SYS_MMAP, 0, THREAD_STACK, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (stk <= 0) return -1;
    long ret = sc5(SYS_CLONE,
        (long)(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD),
        stk + THREAD_STACK, 0, 0, 0);
    if (ret == 0) { fn(); __builtin_unreachable(); }
    return ret;
}

static void spin_until_val(volatile int *flag, int val, int max) {
    for (int i = 0; i < max && *flag != val; i++)
        __asm__ volatile("pause");
}

/* ── Test 1: Multiple IPC events from different "endpoints" (simulated) ── */
static void test_waitany_multi_source(void) {
    puts("\n[ipc_wait_any]\n");

    event_queue_t eq;
    event_queue_init(&eq);

    /* Simulate messages from 3 different endpoints via data field */
    eq_push(&eq, EQ_IPC_MSG, 0); /* endpoint 0 */
    eq_push(&eq, EQ_IPC_MSG, 1); /* endpoint 1 */
    eq_push(&eq, EQ_IPC_MSG, 2); /* endpoint 2 */

    event_t ev;
    eq_pop(&eq, &ev);
    check_val("multi: first from ep 0", (long)ev.data, 0);
    eq_pop(&eq, &ev);
    check_val("multi: second from ep 1", (long)ev.data, 1);
    eq_pop(&eq, &ev);
    check_val("multi: third from ep 2", (long)ev.data, 2);
}

/* ── Test 2: poll() on multiple pipes — wake on first write ── */
static int poll_pipes[4]; /* [r0, w0, r1, w1] */

static void poll_multi_worker(void) {
    __sync_fetch_and_add(&any_worker_started, 1);

    /* poll both read ends */
    struct test_pollfd pfds[2];
    pfds[0].fd = (int)poll_pipes[0]; pfds[0].events = T_POLLIN; pfds[0].revents = 0;
    pfds[1].fd = (int)poll_pipes[2]; pfds[1].events = T_POLLIN; pfds[1].revents = 0;

    long r = sc3(SYS_POLL, (long)pfds, 2, 5000);
    /* Return which pipe was ready: 0 or 1 */
    if (r > 0) {
        if (pfds[0].revents & T_POLLIN) any_worker_result = 0;
        else if (pfds[1].revents & T_POLLIN) any_worker_result = 1;
        else any_worker_result = -2;
    } else {
        any_worker_result = r;
    }
    __sync_fetch_and_add(&any_worker_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_waitany_poll_multi(void) {
    any_worker_done = 0;
    any_worker_result = -999;
    any_worker_started = 0;

    int fds0[2], fds1[2];
    long r = sc2(SYS_PIPE2, (long)fds0, 0);
    check("pipe0 ok", r == 0);
    r = sc2(SYS_PIPE2, (long)fds1, 0);
    check("pipe1 ok", r == 0);

    poll_pipes[0] = fds0[0]; poll_pipes[1] = fds0[1];
    poll_pipes[2] = fds1[0]; poll_pipes[3] = fds1[1];

    long tid = spawn_thread(poll_multi_worker);
    check("spawn poll worker", tid > 0);
    if (tid <= 0) goto cleanup;

    spin_until_val(&any_worker_started, 1, 5000000);
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Write to pipe 1 (second pipe) */
    char c = 'Y';
    sc3(SYS_WRITE, fds1[1], (long)&c, 1);

    spin_until_val(&any_worker_done, 1, 5000000);
    check_val("poll woke on pipe 1", any_worker_result, 1);

cleanup:
    sc1(SYS_CLOSE, fds0[0]); sc1(SYS_CLOSE, fds0[1]);
    sc1(SYS_CLOSE, fds1[0]); sc1(SYS_CLOSE, fds1[1]);
}

/* ── Test 3: poll() wake on first pipe (of two) ── */
static void poll_first_worker(void) {
    __sync_fetch_and_add(&any_worker_started, 1);

    struct test_pollfd pfds[2];
    pfds[0].fd = (int)poll_pipes[0]; pfds[0].events = T_POLLIN; pfds[0].revents = 0;
    pfds[1].fd = (int)poll_pipes[2]; pfds[1].events = T_POLLIN; pfds[1].revents = 0;

    long r = sc3(SYS_POLL, (long)pfds, 2, 5000);
    if (r > 0 && (pfds[0].revents & T_POLLIN))
        any_worker_result = 0;
    else
        any_worker_result = -1;
    __sync_fetch_and_add(&any_worker_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_waitany_poll_first(void) {
    any_worker_done = 0;
    any_worker_result = -999;
    any_worker_started = 0;

    int fds0[2], fds1[2];
    sc2(SYS_PIPE2, (long)fds0, 0);
    sc2(SYS_PIPE2, (long)fds1, 0);

    poll_pipes[0] = fds0[0]; poll_pipes[1] = fds0[1];
    poll_pipes[2] = fds1[0]; poll_pipes[3] = fds1[1];

    long tid = spawn_thread(poll_first_worker);
    check("spawn first-pipe worker", tid > 0);
    if (tid <= 0) goto cleanup;

    spin_until_val(&any_worker_started, 1, 5000000);
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    char c = 'A';
    sc3(SYS_WRITE, fds0[1], (long)&c, 1);

    spin_until_val(&any_worker_done, 1, 5000000);
    check_val("poll woke on pipe 0", any_worker_result, 0);

cleanup:
    sc1(SYS_CLOSE, fds0[0]); sc1(SYS_CLOSE, fds0[1]);
    sc1(SYS_CLOSE, fds1[0]); sc1(SYS_CLOSE, fds1[1]);
}

/* ── Test 4: EQ drain with mixed IPC and non-IPC events ── */
static void test_waitany_eq_mixed_drain(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    eq_push(&eq, EQ_SOCKET_DATA, 1);
    eq_push(&eq, EQ_IPC_MSG, 2);
    eq_push(&eq, EQ_SOCKET_DATA, 3);
    eq_push(&eq, EQ_IPC_MSG, 4);

    event_t drained[4];
    int count = event_drain(&eq, EQ_IPC_MSG, drained, 4);
    check_val("mixed drain: 2 IPC", (long)count, 2);
    check_val("mixed drain[0]=2", (long)drained[0].data, 2);
    check_val("mixed drain[1]=4", (long)drained[1].data, 4);

    /* Remaining: 2 SOCKET_DATA */
    check_val("mixed: remaining=2", (long)event_pending(&eq), 2);
}

/* ── Test 5: poll non-blocking (timeout=0) on empty pipe ── */
static void test_waitany_poll_nonblock(void) {
    int fds[2];
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check("pipe for nonblock poll ok", r == 0);
    if (r != 0) return;

    /* Non-blocking poll: timeout=0 should return immediately with 0 */
    struct test_pollfd pfd;
    pfd.fd = fds[0]; pfd.events = T_POLLIN; pfd.revents = 0;
    r = sc3(SYS_POLL, (long)&pfd, 1, 0);
    check_val("poll(0) empty = 0", r, 0);

    /* Write data, then poll again — should return 1 */
    char c = 'Q';
    sc3(SYS_WRITE, fds[1], (long)&c, 1);
    pfd.revents = 0;
    r = sc3(SYS_POLL, (long)&pfd, 1, 0);
    check_val("poll(0) data = 1", r, 1);
    check("poll(0) POLLIN set", (pfd.revents & T_POLLIN) != 0);

    sc1(SYS_CLOSE, fds[0]);
    sc1(SYS_CLOSE, fds[1]);
}

/* ── Test 6: EQ_IPC_MSG interleaved push/pop ── */
static void test_waitany_eq_interleave(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    eq_push(&eq, EQ_IPC_MSG, 10);
    event_t ev;
    eq_pop(&eq, &ev);
    check_val("interleave: first pop=10", (long)ev.data, 10);

    eq_push(&eq, EQ_IPC_MSG, 20);
    eq_push(&eq, EQ_IPC_MSG, 30);
    eq_pop(&eq, &ev);
    check_val("interleave: second pop=20", (long)ev.data, 20);
    eq_pop(&eq, &ev);
    check_val("interleave: third pop=30", (long)ev.data, 30);
    check_val("interleave: empty after", (long)event_pending(&eq), 0);
}

/* ── Test 7: epoll multi-fd wake ── */
static void test_waitany_epoll_multi(void) {
    int fds0[2], fds1[2];
    long r = sc2(SYS_PIPE2, (long)fds0, 0);
    check("epoll pipe0", r == 0);
    r = sc2(SYS_PIPE2, (long)fds1, 0);
    check("epoll pipe1", r == 0);

    long efd = sc1(SYS_EPOLL_CREATE1, 0);
    check("epoll create", efd >= 0);
    if (efd < 0) goto cleanup7;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = 100;
    sc4(SYS_EPOLL_CTL, efd, EPOLL_CTL_ADD, fds0[0], (long)&ev);
    ev.data = 200;
    sc4(SYS_EPOLL_CTL, efd, EPOLL_CTL_ADD, fds1[0], (long)&ev);

    /* Write to pipe1 */
    char c = 'Z';
    sc3(SYS_WRITE, fds1[1], (long)&c, 1);

    struct epoll_event out[2];
    r = sc4(SYS_EPOLL_WAIT, efd, (long)out, 2, 100);
    check_ge("epoll_wait >= 1", r, 1);
    /* Find pipe1's event */
    int found_200 = 0;
    for (int i = 0; i < (int)r; i++)
        if (out[i].data == 200) found_200 = 1;
    check("epoll woke for pipe1 (data=200)", found_200);

    sc1(SYS_CLOSE, efd);
cleanup7:
    sc1(SYS_CLOSE, fds0[0]); sc1(SYS_CLOSE, fds0[1]);
    sc1(SYS_CLOSE, fds1[0]); sc1(SYS_CLOSE, fds1[1]);
}

/* ── Test 8: EQ empty pop returns -1 ── */
static void test_waitany_eq_empty(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    event_t ev;
    int r = eq_pop(&eq, &ev);
    check_val("empty pop = -1", (long)r, -1);
    check_val("empty: pending=0", (long)event_pending(&eq), 0);
}

/* ── Test 9: poll on eventfd + pipe (multi-type wait) ── */
static void test_waitany_mixed_fds(void) {
    int pfds[2];
    long r = sc2(SYS_PIPE2, (long)pfds, 0);
    check("mixed pipe ok", r == 0);

    long efd = sc2(SYS_EVENTFD2, 0, (long)EFD_NONBLOCK);
    check("mixed eventfd ok", efd >= 0);

    /* Write to eventfd */
    uint64_t val = 1;
    sc3(SYS_WRITE, efd, (long)&val, 8);

    struct test_pollfd polls[2];
    polls[0].fd = pfds[0]; polls[0].events = T_POLLIN; polls[0].revents = 0;
    polls[1].fd = (int)efd; polls[1].events = T_POLLIN; polls[1].revents = 0;

    r = sc3(SYS_POLL, (long)polls, 2, 100);
    check_ge("mixed poll >= 1", r, 1);
    check("eventfd ready", (polls[1].revents & T_POLLIN) != 0);

    sc1(SYS_CLOSE, pfds[0]); sc1(SYS_CLOSE, pfds[1]);
    sc1(SYS_CLOSE, efd);
}

/* ── Test 10: EQ wraparound with IPC msgs ── */
static void test_waitany_eq_wrap(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    /* Fill and drain twice to force wraparound */
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < EQ_MAX_EVENTS; i++)
            eq_push(&eq, EQ_IPC_MSG, (uint64_t)(round * 100 + i));
        for (int i = 0; i < EQ_MAX_EVENTS; i++) {
            event_t ev;
            eq_pop(&eq, &ev);
        }
    }
    check_val("wrap: empty after 2 rounds", (long)event_pending(&eq), 0);

    /* Push one more after wraparound */
    eq_push(&eq, EQ_IPC_MSG, 999);
    event_t ev;
    int r = eq_pop(&eq, &ev);
    check_val("wrap: pop after wrap ok", (long)r, 0);
    check_val("wrap: data=999", (long)ev.data, 999);
}

TEST("ipc_waitany_multi_src", test_waitany_multi_source);
TEST("ipc_waitany_poll_multi", test_waitany_poll_multi);
TEST("ipc_waitany_poll_first", test_waitany_poll_first);
TEST("ipc_waitany_eq_drain", test_waitany_eq_mixed_drain);
TEST("ipc_waitany_poll_nonblock", test_waitany_poll_nonblock);
TEST("ipc_waitany_eq_interleave", test_waitany_eq_interleave);
TEST("ipc_waitany_epoll_multi", test_waitany_epoll_multi);
TEST("ipc_waitany_eq_empty", test_waitany_eq_empty);
TEST("ipc_waitany_mixed_fds", test_waitany_mixed_fds);
TEST("ipc_waitany_eq_wrap", test_waitany_eq_wrap);
