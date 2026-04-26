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

#define WQ_FLAG_EXCLUSIVE  0x01
#define WQ_FLAG_AUTOREMOVE 0x02

/* Wake-Callback. Linux's wait_queue_func_t. Default: autoremove_wake_function.
 * Subsysteme wie epoll registrieren eigene Funktionen, die statt sched_add
 * z.B. ein Ready-Bit setzen und den epoll-eigenen Sleeper wecken. */
typedef int (*wait_func_t)(struct wait_queue_entry *e, unsigned int mask);

typedef struct wait_queue_entry {
    struct thread           *task;
    unsigned int             flags;
    wait_func_t              func;
    struct wait_queue_entry *next;
    struct wait_queue_entry *prev;
} wait_queue_entry_t;

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

/* ── Wake-Primitive (mask-filtered, public) ──────
 * try_to_wake_up: einzige Wake-Funktion. Prueft (1u << t->state) & mask;
 * bei Match CAS state -> RUNNABLE + sched_add. Returns 1 bei Wake, 0 sonst.
 * Caller haelt KEINE Locks (Lock-Akquisition durch wake_up* ueber wq->lock). */
int try_to_wake_up(struct thread *t, unsigned int mask);

/* Default-Funktion: nutzt die Convenience-Felder e->task. */
int default_wake_function(wait_queue_entry_t *e, unsigned int mask);

/* Default fuer DEFINE_WAIT: weckt + markiert Eintrag fuer finish_wait-Cleanup. */
int autoremove_wake_function(wait_queue_entry_t *e, unsigned int mask);

/* Signal-Wake. Wrapper fuer try_to_wake_up(t, TASK_INTERRUPTIBLE | TASK_KILLABLE).
 * Semantik: nur Threads in interruptible Sleep wecken — Uninterruptible (D)
 * bleibt schlafend. Bei uns kollabiert das auf denselben State, aber der
 * Wrapper drueckt die Intent aus, die kill_one in 10.2b uebernimmt. */
void signal_wake_up(struct thread *t);

/* ── DEFINE_WAIT helpers ──────────────────────── */
#define DEFINE_WAIT(name)                                  \
    wait_queue_entry_t name = {                            \
        .task = thread_current(),                          \
        .flags = 0,                                        \
        .func = autoremove_wake_function,                  \
        .next = 0, .prev = 0                               \
    }

#define DEFINE_WAIT_EXCLUSIVE(name)                        \
    wait_queue_entry_t name = {                            \
        .task = thread_current(),                          \
        .flags = WQ_FLAG_EXCLUSIVE,                        \
        .func = autoremove_wake_function,                  \
        .next = 0, .prev = 0                               \
    }

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
