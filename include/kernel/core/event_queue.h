/* CosmoRT Event Queue — per-thread lock-free event delivery
 *
 * Solves the THREAD_BLOCKED race: event_post writes the event BEFORE
 * calling sched_wake. If the target isn't sleeping yet, it finds the
 * event on next event_wait. If already sleeping, sched_wake wakes it.
 *
 * MPSC ringbuffer: multiple producers (serialized by thread->eq_lock),
 * single consumer (owner thread). Monotonic head/tail, power-of-2 masking.
 *
 * Unbounded: ring grows on overflow (page-backed, power-of-2 capacity).
 * IRQ-safe: event_post can be called from IRQ context (RT-Core).
 */
#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdint.h>
#include "core/waitqueue.h"

/* ── Event Types ─────────────────────────────── */

#define EQ_CHILD_EXITED    1
#define EQ_CHILD_STOPPED   2
#define EQ_CHILD_CONTINUED 3
#define EQ_PIPE_DATA       4
#define EQ_PIPE_CLOSED     5
#define EQ_FUTEX_WAKE      6
#define EQ_SOCKET_DATA     8
#define EQ_SOCKET_CONNECT  9
#define EQ_TIMEOUT         10

/* ── Event struct ────────────────────────────── */

typedef struct {
    uint32_t type;
    uint64_t data;
} event_t;

/* ── Event Queue ─────────────────────────────── */

/* Initial ring capacity in events. One 4KB page holds 256 events
 * (sizeof(event_t) = 16). Grows in powers of two via page_alloc on overflow. */
#define EQ_INIT_CAPACITY 256

typedef struct {
    event_t          *events;
    uint32_t          capacity;   /* always power of 2 */
    uint32_t          mask;       /* capacity - 1 */
    volatile uint32_t head;       /* producer index (monotonic) */
    volatile uint32_t tail;       /* consumer index (monotonic) */
    /* Per-queue waitqueue: event_post wakes via wake_up_interruptible,
     * event_wait blocks via prepare_to_wait/finish_wait on this wq. No more
     * routing through thread->wait_head — every sleeper parks on the same
     * wq as its event source. signal/timeout wakers also target this wq. */
    wait_queue_head_t wq;
} event_queue_t;

/* ── Lifecycle (implemented in event_queue.c) ── */

void event_queue_init(event_queue_t *eq);
void event_queue_destroy(event_queue_t *eq);

/* Reset head/tail without touching buffer (for exec flush). */
static inline void event_queue_reset(event_queue_t *eq) {
    eq->head = 0;
    eq->tail = 0;
}

/* ── Inline operations (single-threaded / caller-locked use) ── */

static inline int event_pending(event_queue_t *eq) {
    uint32_t h = eq->head;
    __asm__ volatile("" ::: "memory");
    uint32_t t = eq->tail;
    return (int)(h - t);
}

/* Raw enqueue — no locking, no growth, no wake. For tests or caller-locked use.
 * Full queue: drops oldest (test harness relies on this lossy behavior). */
static inline int eq_push(event_queue_t *eq, uint32_t type, uint64_t data) {
    uint32_t h = eq->head;
    uint32_t t = eq->tail;

    if (h - t >= eq->capacity)
        eq->tail = t + 1;

    eq->events[h & eq->mask].type = type;
    eq->events[h & eq->mask].data = data;
    __asm__ volatile("" ::: "memory");
    eq->head = h + 1;
    return 0;
}

static inline int eq_pop(event_queue_t *eq, event_t *out) {
    uint32_t h = eq->head;
    __asm__ volatile("" ::: "memory");
    uint32_t t = eq->tail;

    if (h == t) return -1;

    *out = eq->events[t & eq->mask];
    __asm__ volatile("" ::: "memory");
    eq->tail = t + 1;
    return 0;
}

/* Drain all events of a specific type. Non-matching events compacted back. */
static inline int event_drain(event_queue_t *eq, uint32_t type, event_t *out, int max) {
    uint32_t h = eq->head;
    uint32_t t = eq->tail;
    int count = 0;
    uint32_t new_head = t;

    for (uint32_t i = t; i != h; i++) {
        event_t *e = &eq->events[i & eq->mask];
        if (e->type == type && count < max) {
            out[count++] = *e;
        } else {
            if (new_head != i)
                eq->events[new_head & eq->mask] = *e;
            new_head++;
        }
    }

    eq->head = new_head;
    return count;
}

/* ── Kernel API (implemented in event_queue.c) ── */

/* Post event to target thread's queue + sched_wake.
 * Non-blocking, IRQ-safe. Grows ring on overflow (page-backed). */
struct thread;
void event_post(struct thread *target, uint32_t type, uint64_t data);

/* Wait for next event. Blocks if queue empty.
 * timeout_ms < 0: infinite. timeout_ms == 0: non-blocking (poll).
 * timeout_ms > 0: sleep with deadline.
 * Returns 0 on success (*out filled), -EAGAIN (empty, non-blocking),
 * -ETIMEDOUT (deadline expired). */
int event_wait(event_queue_t *eq, event_t *out, int timeout_ms);

/* Block current thread for timeout_ms milliseconds (preemptible sleep).
 * Does not touch the event queue — purely time-based blocking.
 * On timeout, syscall restarts. For userspace nanosleep/clock_nanosleep. */
void thread_block_ms(int timeout_ms);

#endif /* EVENT_QUEUE_H */
