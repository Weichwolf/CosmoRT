/* CosmoRT Waitqueue — atomic blocking primitive
 *
 * Linux-style wait_queue_head_t with prepare_to_wait/finish_wait pattern.
 * Serializes state-transition and queue-membership under a single lock
 * so wake_up cannot miss a waiter: either the waker sees the entry and
 * wakes it, or the waiter sees the condition and skips the sleep.
 *
 * Closes the missed-wakeup race that 3x reverted patches tried to fix
 * with CAS/flag tricks in thread_block_ms: the proper fix is lock-serialized
 * state+queue transitions, not more atomics on thread->state.
 *
 * Usage:
 *   wait_queue_head_t wq;
 *   init_waitqueue_head(&wq);
 *   // waiter
 *   DEFINE_WAIT(ent);
 *   for (;;) {
 *       prepare_to_wait(&wq, &ent, THREAD_BLOCKED);
 *       if (condition) break;
 *       schedule();
 *   }
 *   finish_wait(&wq, &ent);
 *   // waker
 *   wake_up(&wq);
 */
#ifndef WAITQUEUE_H
#define WAITQUEUE_H

#include <stdint.h>
#include "spinlock.h"

struct thread;
struct wait_queue_entry;

#define WQ_FLAG_EXCLUSIVE 0x01

/* TASK_*-state masks for try_to_wake_up (bit positions match THREAD_*).
 * THREAD_BLOCKED=3, THREAD_STOPPED=5 → masks pick which sleep states a
 * waker accepts. Linux split BLOCKED into INTERRUPTIBLE / UNINTERRUPTIBLE;
 * we treat THREAD_BLOCKED as INTERRUPTIBLE (the loop callers re-check
 * signal_deliverable themselves, so spurious-wake is the design). */
#define TASK_INTERRUPTIBLE_BIT  (1u << 3)   /* THREAD_BLOCKED */
#define TASK_STOPPED_BIT        (1u << 5)   /* THREAD_STOPPED (SIGCONT) */
#define TASK_NORMAL             (TASK_INTERRUPTIBLE_BIT | TASK_STOPPED_BIT)
#define TASK_ALL                0xffffffffu

/* Wait-queue entry callback: invoked by wake_up to wake one waiter.
 * Return 1 if the wake transitioned a sleeping task → runnable, else 0.
 * Linux: typedef int (*wait_queue_func_t)(struct wait_queue_entry *,
 *                                         unsigned mode, int flags, void *key) */
typedef int (*wait_func_t)(struct wait_queue_entry *e, unsigned int mode,
                           int flags, void *key);

typedef struct wait_queue_entry {
    struct thread           *task;     /* default-func payload */
    unsigned int             flags;
    wait_func_t              func;     /* NULL → default_wake_function */
    struct wait_queue_entry *next;
    struct wait_queue_entry *prev;
} wait_queue_entry_t;

/* Default callback: try_to_wake_up(e->task, mode). Used when entry->func
 * is NULL (the common case for prepare_to_wait/DEFINE_WAIT). */
int default_wake_function(wait_queue_entry_t *e, unsigned int mode,
                          int flags, void *key);

typedef struct wait_queue_head {
    spinlock_t          lock;
    wait_queue_entry_t *head;   /* circular doubly-linked list; NULL = empty */
} wait_queue_head_t;

#define WAIT_QUEUE_HEAD_INIT { .lock = SPINLOCK_INIT, .head = 0 }

/* ── Lifecycle ─────────────────────────────────── */
void init_waitqueue_head(wait_queue_head_t *wq);

/* ── Queue management ─────────────────────────── */
void add_wait_queue(wait_queue_head_t *wq, wait_queue_entry_t *e);
void add_wait_queue_exclusive(wait_queue_head_t *wq, wait_queue_entry_t *e);
void remove_wait_queue(wait_queue_head_t *wq, wait_queue_entry_t *e);

/* ── Atomic state-transition + queue insertion (Linux: prepare_to_wait) ──
 * Takes the waitqueue lock, inserts entry if not already queued, sets
 * current thread state under the lock. Waker must also hold the lock
 * when reading state, which guarantees no missed-wakeup. */
void prepare_to_wait(wait_queue_head_t *wq, wait_queue_entry_t *e, int state);
void prepare_to_wait_exclusive(wait_queue_head_t *wq, wait_queue_entry_t *e, int state);

/* Clear state and dequeue. Idempotent. Must be called before returning
 * from any wait loop (even on signal/timeout paths). */
void finish_wait(wait_queue_head_t *wq, wait_queue_entry_t *e);

/* ── Wakeup ─────────────────────────────────────
 * wake_up: wake all non-exclusive, plus at most one exclusive waiter.
 * wake_up_all: wake everyone (equivalent if no exclusives).
 * wake_up_one: wake exactly one waiter (head first).
 * wake_up_interruptible: same as wake_up (interruptible=informational).
 * Return: number of threads actually transitioned BLOCKED->RUNNABLE. */
int wake_up(wait_queue_head_t *wq);
int wake_up_all(wait_queue_head_t *wq);
int wake_up_one(wait_queue_head_t *wq);
int wake_up_nr(wait_queue_head_t *wq, int n);
int wake_up_interruptible(wait_queue_head_t *wq);

/* ── schedule_timeout family ─────────────────────
 * Block current thread until either wake_up fires on `wq`, timeout
 * elapses, or (interruptible variant) a signal becomes deliverable.
 * timeout_ns == 0 -> infinite. Returns remaining ns (may be 0).
 * For _interruptible: -EINTR on signal, -ETIMEDOUT on timeout, 0 on wake. */
long schedule_timeout(wait_queue_head_t *wq, wait_queue_entry_t *e,
                      uint64_t timeout_ns);
long schedule_timeout_interruptible(wait_queue_head_t *wq, wait_queue_entry_t *e,
                                    uint64_t timeout_ns);

/* Pure sleep until timeout_ns (absolute monotonic) or signal.
 * Internally uses a private waitqueue. Returns 0 on full sleep,
 * -EINTR on signal, -ETIMEDOUT only if timeout_ns == 0 (never-fire).
 * Replaces legacy thread_block_ms. */
int sleep_interruptible_ns(uint64_t timeout_ns);

/* ── DEFINE_WAIT helpers ──────────────────────── */
#define DEFINE_WAIT(name)                                  \
    wait_queue_entry_t name = {                            \
        .task = thread_current(),                          \
        .flags = 0, .func = 0, .next = 0, .prev = 0        \
    }

#define DEFINE_WAIT_EXCLUSIVE(name)                        \
    wait_queue_entry_t name = {                            \
        .task = thread_current(),                          \
        .flags = WQ_FLAG_EXCLUSIVE, .func = 0,             \
        .next = 0, .prev = 0                               \
    }

/* ── try_to_wake_up — single wake primitive ──────
 * CAS t->state from any sleep state in `state_mask` to RUNNABLE and
 * enqueue on rq. No wait-queue routing — pure state machine.
 * Returns 1 if a wake happened, 0 if state did not match mask.
 * Linux: kernel/sched/core.c::try_to_wake_up. */
int try_to_wake_up(struct thread *t, unsigned int state_mask);

/* ── wait_event_interruptible: loop that waits for `cond` ──
 * Evaluates cond under prepare_to_wait; re-checks after schedule.
 * Returns 0 when cond true, -EINTR on signal. */
#define __wait_event_interruptible(wq, cond, ret) do {          \
    DEFINE_WAIT(__ent);                                         \
    for (;;) {                                                  \
        prepare_to_wait(&(wq), &__ent, 3 /* THREAD_BLOCKED */); \
        if (cond) { (ret) = 0; break; }                         \
        if (signal_deliverable()) { (ret) = -4; break; }        \
        schedule();                                             \
    }                                                           \
    finish_wait(&(wq), &__ent);                                 \
} while (0)

int signal_deliverable(void);

#endif /* WAITQUEUE_H */
