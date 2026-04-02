/* CosmoRT High-Resolution Timer — tickless, LAPIC one-shot
 *
 * RB-tree sorted by deadline. LAPIC programmed to next expiry.
 * No periodic tick — zero overhead when idle.
 *
 * Usage:
 *   hrtimer_t t;
 *   hrtimer_init(&t, callback, data);
 *   hrtimer_start(&t, deadline_ns);   // absolute monotonic ns
 *   hrtimer_cancel(&t);
 */
#ifndef HRTIMER_H
#define HRTIMER_H

#include <stdint.h>
#include "core/rbtree.h"
#include "spinlock.h"

/* Timer states */
#define HRTIMER_INACTIVE  0
#define HRTIMER_ENQUEUED  1

typedef struct hrtimer {
    rb_node_t    node;          /* RB-tree linkage */
    uint64_t     deadline_ns;   /* absolute monotonic deadline */
    void       (*fn)(struct hrtimer *);
    void        *data;          /* caller context */
    int          state;
} hrtimer_t;

/* Per-CPU timer base */
typedef struct hrtimer_base {
    rb_root_t    tree;
    spinlock_t   lock;
} hrtimer_base_t;

/* Init subsystem (called once from main) */
void hrtimer_init_subsystem(void);

/* Init a timer struct (caller allocates) */
void hrtimer_init(hrtimer_t *t, void (*fn)(hrtimer_t *), void *data);

/* Start timer with absolute monotonic deadline in nanoseconds.
 * If already enqueued, moves to new deadline. */
void hrtimer_start(hrtimer_t *t, uint64_t deadline_ns);

/* Cancel a pending timer. No-op if inactive. Returns 1 if was active. */
int hrtimer_cancel(hrtimer_t *t);

/* Current monotonic time in nanoseconds (TSC-based). */
uint64_t hrtimer_now_ns(void);

/* Called from timer IRQ: fire expired timers, reprogram LAPIC. */
void hrtimer_run_expired(void);

/* Program LAPIC to fire at the next pending deadline.
 * Called after insert/cancel and from idle. */
void hrtimer_reprogram(void);

/* Convenience: nanoseconds from milliseconds */
#define NSEC_PER_MSEC  1000000ULL
#define NSEC_PER_SEC   1000000000ULL

static inline uint64_t ms_to_ns(uint64_t ms) { return ms * NSEC_PER_MSEC; }

#endif
