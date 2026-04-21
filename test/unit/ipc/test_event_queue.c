#include "ktest.h"

#include "core/event_queue.h"

/* Userspace uses a local buffer — kernel-side event_queue_init (which
 * allocates pages) would not link here. Integration with event_post /
 * event_wait is covered through fork+wait4 and nanosleep below. */
#define TEST_EQ_CAP 64

static void eq_setup(event_queue_t *eq, event_t *buf, uint32_t cap) {
    eq->events = buf;
    eq->capacity = cap;
    eq->mask = cap - 1;
    eq->head = 0;
    eq->tail = 0;
}

static void test_event_queue(void) {
    puts("\n[Event Queue]\n");

    event_t buf[TEST_EQ_CAP];
    event_queue_t eq;

    eq_setup(&eq, buf, TEST_EQ_CAP);
    check_val("init: pending=0", (long)event_pending(&eq), 0);

    eq_push(&eq, EQ_CHILD_EXITED, 42);
    check_val("push: pending=1", (long)event_pending(&eq), 1);

    event_t ev;
    int r = eq_pop(&eq, &ev);
    check_val("pop: return 0", (long)r, 0);
    check_val("pop: type=CHILD_EXITED", (long)ev.type, EQ_CHILD_EXITED);
    check_val("pop: data=42", (long)ev.data, 42);
    check_val("pop: pending=0", (long)event_pending(&eq), 0);

    r = eq_pop(&eq, &ev);
    check_val("pop empty: return -1", (long)r, -1);

    eq_push(&eq, EQ_PIPE_DATA, 100);
    eq_push(&eq, EQ_FUTEX_WAKE, 200);
    check_val("2 push: pending=2", (long)event_pending(&eq), 2);

    r = eq_pop(&eq, &ev);
    check_val("fifo pop 1: return 0", (long)r, 0);
    check_val("fifo pop 1: type=PIPE_DATA", (long)ev.type, EQ_PIPE_DATA);
    check_val("fifo pop 1: data=100", (long)ev.data, 100);

    r = eq_pop(&eq, &ev);
    check_val("fifo pop 2: return 0", (long)r, 0);
    check_val("fifo pop 2: type=FUTEX_WAKE", (long)ev.type, EQ_FUTEX_WAKE);
    check_val("fifo pop 2: data=200", (long)ev.data, 200);

    eq_setup(&eq, buf, TEST_EQ_CAP);
    for (int i = 0; i < 8; i++)
        eq_push(&eq, EQ_SOCKET_DATA, (uint64_t)i);
    check_val("8 push: pending=8", (long)event_pending(&eq), 8);

    int fifo_ok = 1;
    for (int i = 0; i < 8; i++) {
        eq_pop(&eq, &ev);
        if (ev.type != EQ_SOCKET_DATA || ev.data != (uint64_t)i)
            fifo_ok = 0;
    }
    check("8 events FIFO order", fifo_ok);

    /* Raw eq_push is the no-grow path: beyond capacity it drops oldest. */
    eq_setup(&eq, buf, TEST_EQ_CAP);
    for (int i = 0; i < TEST_EQ_CAP + 4; i++)
        eq_push(&eq, EQ_TIMEOUT, (uint64_t)i);

    check_val("overflow: pending=capacity", (long)event_pending(&eq), (long)TEST_EQ_CAP);
    eq_pop(&eq, &ev);
    check_val("overflow: first pop data=4", (long)ev.data, 4);

    eq_setup(&eq, buf, TEST_EQ_CAP);
    eq_push(&eq, EQ_PIPE_DATA, 10);
    eq_push(&eq, EQ_CHILD_EXITED, 20);
    eq_push(&eq, EQ_PIPE_DATA, 30);
    eq_push(&eq, EQ_FUTEX_WAKE, 40);
    eq_push(&eq, EQ_PIPE_DATA, 50);

    event_t drained[8];
    int count = event_drain(&eq, EQ_PIPE_DATA, drained, 8);
    check_val("drain PIPE_DATA: count=3", (long)count, 3);
    check_val("drain[0].data=10", (long)drained[0].data, 10);
    check_val("drain[1].data=30", (long)drained[1].data, 30);
    check_val("drain[2].data=50", (long)drained[2].data, 50);

    check_val("after drain: pending=2", (long)event_pending(&eq), 2);
    eq_pop(&eq, &ev);
    check_val("after drain: first=CHILD_EXITED", (long)ev.type, EQ_CHILD_EXITED);
    check_val("after drain: first.data=20", (long)ev.data, 20);
    eq_pop(&eq, &ev);
    check_val("after drain: second=FUTEX_WAKE", (long)ev.type, EQ_FUTEX_WAKE);
    check_val("after drain: second.data=40", (long)ev.data, 40);

    eq_setup(&eq, buf, TEST_EQ_CAP);
    for (int i = 0; i < 6; i++)
        eq_push(&eq, EQ_SOCKET_DATA, (uint64_t)(i + 1));

    count = event_drain(&eq, EQ_SOCKET_DATA, drained, 2);
    check_val("drain max=2: count=2", (long)count, 2);
    check_val("drain max=2: [0].data=1", (long)drained[0].data, 1);
    check_val("drain max=2: [1].data=2", (long)drained[1].data, 2);
    check_val("drain max=2: remaining=4", (long)event_pending(&eq), 4);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT, 7);
    }
    check("fork succeeded", pid > 0);

    int wstatus = 0;
    long wp = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child pid", wp, pid);
    check_val("wait4 exit code 7", (long)((wstatus >> 8) & 0xFF), 7);

    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    struct k_timespec before, after;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&before);
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&after);
    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
                    + (after.tv_nsec - before.tv_nsec) / 1000000;
    check("nanosleep 10ms elapsed >= 5ms", elapsed_ms >= 5);
}

TEST("event_queue", test_event_queue);
