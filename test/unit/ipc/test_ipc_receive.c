/* IPC receive: event_wait blocking semantics tests
 *
 * IPC is kernel-internal (no syscall). These tests validate the
 * event_wait/event_post mechanism that ipc_recv now uses, via:
 * - EQ_IPC_MSG event type in the inline event queue
 * - Pipe blocking (uses same event_post wake pattern)
 * - Thread wake-from-block correctness
 */
#include "ktest.h"
#include "core/event_queue.h"

#define THREAD_STACK 65536

/* ── Shared state ── */
static volatile int recv_worker_done;
static volatile long recv_worker_result;
static volatile int recv_worker_started;

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

/* ── Test 1: EQ_IPC_MSG push/pop roundtrip ── */
static void test_ipc_eq_roundtrip(void) {
    puts("\n[ipc_receive]\n");

    event_queue_t eq;
    event_queue_init(&eq);
    eq_push(&eq, EQ_IPC_MSG, 42);
    check_val("eq: pending after IPC push", (long)event_pending(&eq), 1);

    event_t ev;
    int r = eq_pop(&eq, &ev);
    check_val("eq: pop IPC msg ok", (long)r, 0);
    check_val("eq: type=EQ_IPC_MSG", (long)ev.type, EQ_IPC_MSG);
    check_val("eq: data=42", (long)ev.data, 42);
}

/* ── Test 2: EQ_IPC_MSG does not interfere with other types ── */
static void test_ipc_eq_isolation(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    eq_push(&eq, EQ_PIPE_DATA, 1);
    eq_push(&eq, EQ_IPC_MSG, 2);
    eq_push(&eq, EQ_FUTEX_WAKE, 3);
    check_val("eq: 3 events pending", (long)event_pending(&eq), 3);

    event_t ev;
    eq_pop(&eq, &ev);
    check_val("eq: first=PIPE_DATA", (long)ev.type, EQ_PIPE_DATA);
    eq_pop(&eq, &ev);
    check_val("eq: second=IPC_MSG", (long)ev.type, EQ_IPC_MSG);
    eq_pop(&eq, &ev);
    check_val("eq: third=FUTEX_WAKE", (long)ev.type, EQ_FUTEX_WAKE);
}

/* ── Test 3: EQ_IPC_MSG drain selective ── */
static void test_ipc_eq_drain(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    eq_push(&eq, EQ_IPC_MSG, 10);
    eq_push(&eq, EQ_PIPE_DATA, 20);
    eq_push(&eq, EQ_IPC_MSG, 30);

    event_t drained[4];
    int count = event_drain(&eq, EQ_IPC_MSG, drained, 4);
    check_val("drain IPC: count=2", (long)count, 2);
    check_val("drain IPC[0].data=10", (long)drained[0].data, 10);
    check_val("drain IPC[1].data=30", (long)drained[1].data, 30);
    check_val("drain: remaining=1", (long)event_pending(&eq), 1);
}

/* ── Test 4: Pipe blocking read wakes via event_post (same pattern as IPC) ── */
static int pipe_fds[2];

static void pipe_reader_worker(void) {
    __sync_fetch_and_add(&recv_worker_started, 1);
    char buf[1];
    /* Blocking read — will block via event_wait, woken by pipe_write's event_post */
    long n = sc3(SYS_READ, pipe_fds[0], (long)buf, 1);
    recv_worker_result = n;
    __sync_fetch_and_add(&recv_worker_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_ipc_pipe_block_wake(void) {
    recv_worker_done = 0;
    recv_worker_result = -999;
    recv_worker_started = 0;

    long r = sc2(SYS_PIPE2, (long)pipe_fds, 0);
    check("pipe create ok", r == 0);
    if (r != 0) return;

    long tid = spawn_thread(pipe_reader_worker);
    check("spawn reader", tid > 0);
    if (tid <= 0) { sc1(SYS_CLOSE, pipe_fds[0]); sc1(SYS_CLOSE, pipe_fds[1]); return; }

    spin_until_val(&recv_worker_started, 1, 5000000);
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    char c = 'X';
    sc3(SYS_WRITE, pipe_fds[1], (long)&c, 1);

    spin_until_val(&recv_worker_done, 1, 5000000);
    check_val("pipe read woke: got 1 byte", recv_worker_result, 1);

    sc1(SYS_CLOSE, pipe_fds[0]);
    sc1(SYS_CLOSE, pipe_fds[1]);
}

/* ── Test 5: Pipe blocking read — EOF on close (event_post with PIPE_CLOSED) ── */
static void pipe_eof_worker(void) {
    __sync_fetch_and_add(&recv_worker_started, 1);
    char buf[1];
    long n = sc3(SYS_READ, pipe_fds[0], (long)buf, 1);
    recv_worker_result = n; /* 0 = EOF */
    __sync_fetch_and_add(&recv_worker_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_ipc_pipe_eof(void) {
    recv_worker_done = 0;
    recv_worker_result = -999;
    recv_worker_started = 0;

    long r = sc2(SYS_PIPE2, (long)pipe_fds, 0);
    check("pipe for eof ok", r == 0);
    if (r != 0) return;

    long tid = spawn_thread(pipe_eof_worker);
    check("spawn eof reader", tid > 0);
    if (tid <= 0) { sc1(SYS_CLOSE, pipe_fds[0]); sc1(SYS_CLOSE, pipe_fds[1]); return; }

    spin_until_val(&recv_worker_started, 1, 5000000);
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    /* Close write end → reader gets EOF */
    sc1(SYS_CLOSE, pipe_fds[1]);

    spin_until_val(&recv_worker_done, 1, 5000000);
    check_val("pipe EOF: read returns 0", recv_worker_result, 0);

    sc1(SYS_CLOSE, pipe_fds[0]);
}

/* ── Test 6: EQ_IPC_MSG overflow behavior ── */
static void test_ipc_eq_overflow(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    for (int i = 0; i < EQ_MAX_EVENTS + 4; i++)
        eq_push(&eq, EQ_IPC_MSG, (uint64_t)i);

    check_val("overflow: pending=16", (long)event_pending(&eq), EQ_MAX_EVENTS);

    event_t ev;
    eq_pop(&eq, &ev);
    check_val("overflow: oldest dropped, first=4", (long)ev.data, 4);
    check_val("overflow: type=IPC_MSG", (long)ev.type, EQ_IPC_MSG);
}

/* ── Test 7: Multiple IPC messages FIFO order ── */
static void test_ipc_eq_fifo(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    for (int i = 0; i < 8; i++)
        eq_push(&eq, EQ_IPC_MSG, (uint64_t)(i + 100));

    int ok = 1;
    for (int i = 0; i < 8; i++) {
        event_t ev;
        eq_pop(&eq, &ev);
        if (ev.type != EQ_IPC_MSG || ev.data != (uint64_t)(i + 100))
            ok = 0;
    }
    check("8 IPC msgs FIFO order", ok);
}

/* ── Test 8: Eventfd non-blocking write+read (same event type pattern) ── */
static void test_ipc_eventfd_readwrite(void) {
    long efd = sc2(SYS_EVENTFD2, 0, (long)EFD_NONBLOCK);
    check("eventfd create", efd >= 0);
    if (efd < 0) return;

    /* Write to eventfd */
    uint64_t val = 7;
    long n = sc3(SYS_WRITE, efd, (long)&val, 8);
    check_val("eventfd write ok", n, 8);

    /* Read back */
    uint64_t rval = 0;
    n = sc3(SYS_READ, efd, (long)&rval, 8);
    check_val("eventfd read ok", n, 8);
    check_val("eventfd val=7", (long)rval, 7);

    /* Second read should be EAGAIN (counter reset to 0) */
    n = sc3(SYS_READ, efd, (long)&rval, 8);
    check_val("eventfd empty = -EAGAIN", n, -EAGAIN);

    sc1(SYS_CLOSE, efd);
}

/* ── Test 9: EQ_IPC_MSG with zero data ── */
static void test_ipc_eq_zero_data(void) {
    event_queue_t eq;
    event_queue_init(&eq);

    eq_push(&eq, EQ_IPC_MSG, 0);
    event_t ev;
    int r = eq_pop(&eq, &ev);
    check_val("IPC zero data: pop ok", (long)r, 0);
    check_val("IPC zero data: type=IPC_MSG", (long)ev.type, EQ_IPC_MSG);
    check_val("IPC zero data: data=0", (long)ev.data, 0);
}

/* ── Test 10: Futex wake uses event_post (same wake pattern as IPC) ── */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128

static volatile uint32_t futex_ipc;
static volatile int futex_ipc_result;

static void futex_ipc_worker(void) {
    __sync_fetch_and_add(&recv_worker_started, 1);
    long r = sc6(SYS_FUTEX, (long)&futex_ipc,
                 FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    futex_ipc_result = (int)r;
    __sync_fetch_and_add(&recv_worker_done, 1);
    sc1(SYS_EXIT, 0);
    __builtin_unreachable();
}

static void test_ipc_futex_wake_pattern(void) {
    recv_worker_done = 0;
    futex_ipc_result = -999;
    recv_worker_started = 0;
    futex_ipc = 1;

    long tid = spawn_thread(futex_ipc_worker);
    check("spawn futex worker", tid > 0);
    if (tid <= 0) return;

    spin_until_val(&recv_worker_started, 1, 5000000);
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);

    futex_ipc = 0;
    __sync_synchronize();
    long woken = sc6(SYS_FUTEX, (long)&futex_ipc,
                     FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    check_ge("futex wake >= 1", woken, 1);

    spin_until_val(&recv_worker_done, 1, 5000000);
    check("futex worker woke", recv_worker_done == 1);
}

TEST("ipc_recv_eq_roundtrip", test_ipc_eq_roundtrip);
TEST("ipc_recv_eq_isolation", test_ipc_eq_isolation);
TEST("ipc_recv_eq_drain", test_ipc_eq_drain);
TEST("ipc_recv_pipe_block", test_ipc_pipe_block_wake);
TEST("ipc_recv_pipe_eof", test_ipc_pipe_eof);
TEST("ipc_recv_eq_overflow", test_ipc_eq_overflow);
TEST("ipc_recv_eq_fifo", test_ipc_eq_fifo);
TEST("ipc_recv_eventfd", test_ipc_eventfd_readwrite);
TEST("ipc_recv_eq_zero", test_ipc_eq_zero_data);
TEST("ipc_recv_futex_wake", test_ipc_futex_wake_pattern);
