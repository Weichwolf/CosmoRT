/* CosmoRT Event Queue — per-thread lock-free event delivery
 *
 * Solves the THREAD_BLOCKED race: event_post writes the event BEFORE
 * calling sched_wake. If the target isn't sleeping yet, it finds the
 * event on next event_wait. If already sleeping, sched_wake wakes it.
 *
 * MPSC ringbuffer: multiple producers (serialized by eq_lock spinlock),
 * single consumer (owner thread). Monotonic head/tail, power-of-2 masking.
 *
 * IRQ-safe: event_post can be called from IRQ context (RT-Core).
 */
#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdint.h>

/* ── Event Types ─────────────────────────────── */

#define EQ_CHILD_EXITED    1
#define EQ_CHILD_STOPPED   2
#define EQ_CHILD_CONTINUED 3
#define EQ_PIPE_DATA       4
#define EQ_PIPE_CLOSED     5
#define EQ_FUTEX_WAKE      6
#define EQ_EPOLL_READY     7
#define EQ_SOCKET_DATA     8
#define EQ_SOCKET_CONNECT  9
#define EQ_TIMEOUT         10
#define EQ_IPC_MSG         11

/* ── Event struct ────────────────────────────── */

typedef struct {
    uint32_t type;
    uint64_t data;
} event_t;

/* ── Event Queue ─────────────────────────────── */

#define EQ_MAX_EVENTS 16
#define EQ_MASK       (EQ_MAX_EVENTS - 1)

typedef struct {
    event_t  events[EQ_MAX_EVENTS];
    volatile uint32_t head;  /* producer index (monotonic) */
    volatile uint32_t tail;  /* consumer index (monotonic) */
} event_queue_t;

/* ── Inline operations (no kernel dependencies, testable in userspace) ── */

static inline void event_queue_init(event_queue_t *eq) {
    eq->head = 0;
    eq->tail = 0;
}

/* Non-blocking: number of pending events. */
static inline int event_pending(event_queue_t *eq) {
    uint32_t h = eq->head;
    __asm__ volatile("" ::: "memory"); /* compiler barrier (load-acquire) */
    uint32_t t = eq->tail;
    return (int)(h - t);
}

/* Raw enqueue — no locking, no wake. For single-producer or caller-locked use. */
static inline int eq_push(event_queue_t *eq, uint32_t type, uint64_t data) {
    uint32_t h = eq->head;
    uint32_t t = eq->tail;

    /* Queue full: advance tail (drop oldest) */
    if (h - t >= EQ_MAX_EVENTS) {
        eq->tail = t + 1;
    }

    eq->events[h & EQ_MASK].type = type;
    eq->events[h & EQ_MASK].data = data;
    __asm__ volatile("" ::: "memory"); /* compiler barrier (store-release) */
    eq->head = h + 1;
    return 0;
}

/* Raw dequeue — single consumer. Returns 0 on success, -1 if empty. */
static inline int eq_pop(event_queue_t *eq, event_t *out) {
    uint32_t h = eq->head;
    __asm__ volatile("" ::: "memory"); /* compiler barrier (load-acquire) */
    uint32_t t = eq->tail;

    if (h == t) return -1;

    *out = eq->events[t & EQ_MASK];
    __asm__ volatile("" ::: "memory"); /* compiler barrier (store-release) */
    eq->tail = t + 1;
    return 0;
}

/* Drain all events of a specific type. Returns count drained (up to max).
 * Non-matching events are compacted back into the queue. */
static inline int event_drain(event_queue_t *eq, uint32_t type, event_t *out, int max) {
    uint32_t h = eq->head;
    uint32_t t = eq->tail;
    int count = 0;
    uint32_t new_head = t;

    for (uint32_t i = t; i != h; i++) {
        event_t *e = &eq->events[i & EQ_MASK];
        if (e->type == type && count < max) {
            out[count++] = *e;
        } else {
            if (new_head != i)
                eq->events[new_head & EQ_MASK] = *e;
            new_head++;
        }
    }

    eq->head = new_head;
    return count;
}

/* ── Kernel API (implemented in event_queue.c) ── */

/* Post event to target thread's queue + sched_wake.
 * Non-blocking, IRQ-safe. Queue full: oldest overwritten. */
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
