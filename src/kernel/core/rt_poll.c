/* CosmoRT BSP Prioritised Polling
 *
 * Single-threaded on BSP (Core 0). No locks.
 * Static registration: fixed array, no malloc.
 *
 * After each handler that did work, re-scan from highest priority.
 * This guarantees audio/input preempt network bursts.
 * Bounded: each handler is max_work-limited per invocation,
 * and total restarts are capped to prevent livelock.
 */

#include "core/rt_poll.h"

/* ── Static handler table ────────────────────────── */

typedef struct {
    rt_poll_fn fn;
    int        max_work;
} rt_poll_slot_t;

static rt_poll_slot_t slots[RT_PRIO_COUNT];

void rt_poll_register(enum rt_prio prio, rt_poll_fn fn, int max_work) {
    if ((unsigned)prio < RT_PRIO_COUNT) {
        slots[prio].fn       = fn;
        slots[prio].max_work = max_work;
    }
}

/* ── Stub handlers for subsystems not yet implemented ── */

static int audio_poll_stub(int max_work) { (void)max_work; return 0; }
static int input_poll_stub(int max_work) { (void)max_work; return 0; }
static int vsync_poll_stub(int max_work) { (void)max_work; return 0; }

void rt_poll_init(void) {
    rt_poll_register(RT_PRIO_AUDIO, audio_poll_stub, 1);
    rt_poll_register(RT_PRIO_INPUT, input_poll_stub, 8);
    rt_poll_register(RT_PRIO_VSYNC, vsync_poll_stub, 1);
}

/* ── Main polling loop ───────────────────────────── */

#define MAX_RESTARTS 4  /* prevent livelock from continuous high-prio work */

void rt_poll_run(void) {
    /* Single-core: no guard needed */

    int restarts = 0;

    for (int p = 0; p < RT_PRIO_COUNT; p++) {
        if (!slots[p].fn) continue;

        int done = slots[p].fn(slots[p].max_work);

        /* If lower-prio handler did work, re-check higher priorities */
        if (done > 0 && p > 0 && restarts < MAX_RESTARTS) {
            restarts++;
            p = -1; /* becomes 0 after p++ */
        }
    }
}

/* ── Test support (called from SYS_COSMO_RT_QUERY) ── */

/* Test call-order tracking: each test handler writes its sequence number.
 * Written on RT-Core (timer IRQ), read from Compute (syscall). */
static volatile int test_seq;
static volatile int test_call_order[RT_PRIO_COUNT];
static volatile int test_max_work_seen[RT_PRIO_COUNT];
static volatile int test_active;

static int test_handler(int prio, int max_work) {
    test_call_order[prio] = ++test_seq;
    test_max_work_seen[prio] = max_work;
    return 0; /* no work, no restart */
}

static int test_handler_0(int max_work) { return test_handler(0, max_work); }
static int test_handler_1(int max_work) { return test_handler(1, max_work); }
static int test_handler_2(int max_work) { return test_handler(2, max_work); }
static int test_handler_3(int max_work) { return test_handler(3, max_work); }
static int test_handler_4(int max_work) { return test_handler(4, max_work); }
static int test_handler_5(int max_work) { return test_handler(5, max_work); }
static int test_handler_6(int max_work) { return test_handler(6, max_work); }
static int test_handler_7(int max_work) { return test_handler(7, max_work); }
static int test_handler_8(int max_work) { return test_handler(8, max_work); }

static rt_poll_fn test_handlers[RT_PRIO_COUNT] = {
    test_handler_0, test_handler_1, test_handler_2,
    test_handler_3, test_handler_4, test_handler_5,
    test_handler_6, test_handler_7, test_handler_8
};

/* Saved real handlers for restore */
static rt_poll_slot_t saved_slots[RT_PRIO_COUNT];

/* Install test handlers, saving originals. Called from syscall (Compute). */
int rt_poll_test_install(void) {
    for (int i = 0; i < RT_PRIO_COUNT; i++) {
        saved_slots[i] = slots[i];
        slots[i].fn = test_handlers[i];
        /* Keep max_work from registration to verify bounded work */
    }
    test_seq = 0;
    for (int i = 0; i < RT_PRIO_COUNT; i++) {
        test_call_order[i] = 0;
        test_max_work_seen[i] = -1;
    }
    test_active = 1;
    return 0;
}

/* Restore real handlers. Called from syscall (Compute). */
int rt_poll_test_restore(void) {
    for (int i = 0; i < RT_PRIO_COUNT; i++)
        slots[i] = saved_slots[i];
    test_active = 0;
    return 0;
}

/* Read test results. Called from syscall (Compute).
 * sub=0: call_order[prio], sub=1: max_work_seen[prio], sub=2: test_seq */
int rt_poll_test_query(int sub, int prio) {
    if (sub == 0 && (unsigned)prio < RT_PRIO_COUNT)
        return test_call_order[prio];
    if (sub == 1 && (unsigned)prio < RT_PRIO_COUNT)
        return test_max_work_seen[prio];
    if (sub == 2) return test_seq;
    return -1;
}
