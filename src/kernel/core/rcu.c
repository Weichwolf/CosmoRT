/* CosmoRT Preemptible RCU — Linux PREEMPT_RCU model
 *
 * Grace period state machine:
 *   GP_IDLE (gp_seq even) → rcu_gp_start() → GP_ACTIVE (gp_seq odd)
 *   All QS reported + no blocked readers → rcu_gp_complete() → GP_IDLE
 *
 * Lock ordering (always acquire in this order):
 *   1. rcu_state.gp_lock   — protects gp_seq transitions
 *   2. rcu_node.lock        — protects qsmask + blocked reader list
 */

#include "core/rcu.h"
#include "core/percpu.h"
#include "core/tick.h"
#include "proc/thread.h"
#include "spinlock.h"
#include "hw/serial.h"
#include "config.h"
#include "hal/hal.h"

/* ── Scheduler/process externs (file scope) ──── */

extern void sched_wake(struct thread *t);
extern void save_user_state_for_block(struct thread *t, long return_value);
extern void schedule(void);

/* ── Internal types (not exposed in rcu.h) ───── */

#define RCU_SEQ_IDLE(seq)   (((seq) & 1) == 0)
#define RCU_SEQ_ACTIVE(seq) (((seq) & 1) == 1)

typedef struct rcu_node {
    spinlock_t      lock;
    uint64_t        gp_seq;
    uint32_t        qsmask;         /* bit per CPU: 1 = owes QS */
    uint32_t        qsmaskinit;     /* reset mask for new GP */
    struct thread  *blkd_head;      /* doubly-linked blocked reader list */
    struct thread  *blkd_tail;
    int             blkd_count;
} rcu_node_t;

typedef struct rcu_data {
    uint64_t        gp_seq;
    int             qs_pending;
    rcu_head_t     *cb_wait_head;   /* callbacks awaiting current GP */
    rcu_head_t     *cb_wait_tail;
    rcu_head_t     *cb_next_head;   /* callbacks queued after GP start */
    rcu_head_t     *cb_next_tail;
    rcu_head_t     *cb_done_head;   /* GP ended, awaiting deferred execution */
    rcu_head_t     *cb_done_tail;
    int             cb_count;
} rcu_data_t;

/* synchronize_rcu waiter node (stack-allocated by caller) */
typedef struct rcu_sync_node {
    struct thread          *waiter;
    struct rcu_sync_node   *next;
} rcu_sync_node_t;

typedef struct rcu_state {
    spinlock_t      gp_lock;
    uint64_t        gp_seq;         /* odd=active, even=idle */
    rcu_node_t      node;
    rcu_data_t      cpu[SMP_MAX_CORES];
    rcu_sync_node_t *sync_head;     /* synchronize_rcu active waiter list */
    rcu_sync_node_t *sync_ready;    /* waiters detached by gp_complete, awaiting wake */
    volatile int    defer_pending;  /* tick picks up deferred callbacks + wakes */
} rcu_state_t;

static rcu_state_t rcu_state;

/* Defensive: clamp core_id to a valid rcu_state.cpu[] slot.
 * Out-of-range indicates early-boot race or corrupt percpu — attributing the
 * callback to CPU 0 keeps the GP-contract intact (callbacks still fire after
 * GP end); only per-CPU accounting is imprecise. */
static inline int rcu_cpu_of_self(void) {
    int id = percpu_self()->core_id;
    if (__builtin_expect((unsigned)id >= (unsigned)SMP_MAX_CORES, 0))
        id = 0;
    return id;
}

/* ── Blocked reader list (doubly-linked, O(1) ops) ── */

static void blkd_append(rcu_node_t *rnp, struct thread *t) {
    t->rcu_next = 0;
    t->rcu_prev = rnp->blkd_tail;
    if (rnp->blkd_tail)
        rnp->blkd_tail->rcu_next = t;
    else
        rnp->blkd_head = t;
    rnp->blkd_tail = t;
    rnp->blkd_count++;
    t->rcu_blocked_node = rnp;
}

static void blkd_remove(rcu_node_t *rnp, struct thread *t) {
    if (t->rcu_prev)
        t->rcu_prev->rcu_next = t->rcu_next;
    else
        rnp->blkd_head = t->rcu_next;
    if (t->rcu_next)
        t->rcu_next->rcu_prev = t->rcu_prev;
    else
        rnp->blkd_tail = t->rcu_prev;
    t->rcu_next = 0;
    t->rcu_prev = 0;
    t->rcu_blocked_node = 0;
    rnp->blkd_count--;
}

/* ── Deferred callback execution ─────────────
 *
 * gp_complete detaches finished callbacks into cb_done_* and sets defer_pending.
 * rcu_tick_deferred() drains cb_done_* on the current CPU and wakes sync waiters.
 * This keeps callbacks off the synchronize_rcu / schedule() / rcu_read_unlock
 * call paths that triggered GP completion. */

static void rcu_drain_done(int cpu_id) {
    rcu_data_t *rdp = &rcu_state.cpu[cpu_id];

    uint64_t flags = irq_save();
    rcu_head_t *head = rdp->cb_done_head;
    rdp->cb_done_head = 0;
    rdp->cb_done_tail = 0;
    irq_restore(flags);

    while (head) {
        rcu_head_t *next = head->next;
        head->next = 0;
        rdp->cb_count--;
        head->func(head);
        head = next;
    }
}

static void rcu_tick_deferred(void) {
    if (__builtin_expect(!READ_ONCE(rcu_state.defer_pending), 1))
        return;

    rcu_state.defer_pending = 0;

    for (int i = 0; i < SMP_MAX_CORES; i++) {
        if (rcu_state.cpu[i].cb_done_head)
            rcu_drain_done(i);
    }

    uint64_t flags;
    spin_lock_irq(&rcu_state.gp_lock, &flags);
    rcu_sync_node_t *waiters = rcu_state.sync_ready;
    rcu_state.sync_ready = 0;
    spin_unlock_irq(&rcu_state.gp_lock, flags);

    while (waiters) {
        rcu_sync_node_t *next = waiters->next;
        sched_wake(waiters->waiter);
        waiters = next;
    }
}

static struct tick_callback rcu_tick_cb;

/* ── Grace period start ────────────────────── */

static void rcu_gp_start(void) {
    uint64_t flags;
    spin_lock_irq(&rcu_state.gp_lock, &flags);

    if (RCU_SEQ_ACTIVE(rcu_state.gp_seq)) {
        spin_unlock_irq(&rcu_state.gp_lock, flags);
        return;
    }

    rcu_state.gp_seq++;

    rcu_node_t *rnp = &rcu_state.node;
    uint64_t nflags;
    spin_lock_irq(&rnp->lock, &nflags);
    rnp->gp_seq = rcu_state.gp_seq;
    rnp->qsmask = rnp->qsmaskinit;
    spin_unlock_irq(&rnp->lock, nflags);

    for (int i = 0; i < SMP_MAX_CORES; i++) {
        rcu_data_t *rdp = &rcu_state.cpu[i];
        rdp->gp_seq = rcu_state.gp_seq;
        rdp->qs_pending = 1;

        /* Promote next→wait: callbacks queued before GP started */
        if (rdp->cb_next_head) {
            if (rdp->cb_wait_tail)
                rdp->cb_wait_tail->next = rdp->cb_next_head;
            else
                rdp->cb_wait_head = rdp->cb_next_head;
            rdp->cb_wait_tail = rdp->cb_next_tail;
            rdp->cb_next_head = 0;
            rdp->cb_next_tail = 0;
        }
    }

    spin_unlock_irq(&rcu_state.gp_lock, flags);
}

/* ── Grace period completion ───────────────── */

static void rcu_gp_complete(void) {
    uint64_t flags;
    spin_lock_irq(&rcu_state.gp_lock, &flags);

    if (!RCU_SEQ_ACTIVE(rcu_state.gp_seq)) {
        spin_unlock_irq(&rcu_state.gp_lock, flags);
        return;
    }

    rcu_node_t *rnp = &rcu_state.node;
    if (rnp->qsmask != 0 || rnp->blkd_count != 0) {
        spin_unlock_irq(&rcu_state.gp_lock, flags);
        return;
    }

    rcu_state.gp_seq++;
    rnp->gp_seq = rcu_state.gp_seq;

    /* Move finished cb_wait → cb_done per CPU (FIFO append).
     * done-queue belongs to the tick; callbacks run there, not here. */
    for (int i = 0; i < SMP_MAX_CORES; i++) {
        rcu_data_t *rdp = &rcu_state.cpu[i];
        if (!rdp->cb_wait_head)
            continue;
        if (rdp->cb_done_tail)
            rdp->cb_done_tail->next = rdp->cb_wait_head;
        else
            rdp->cb_done_head = rdp->cb_wait_head;
        rdp->cb_done_tail = rdp->cb_wait_tail;
        rdp->cb_wait_head = 0;
        rdp->cb_wait_tail = 0;
    }

    /* Move active synchronize_rcu waiters → sync_ready (deferred wake). */
    rcu_sync_node_t *waiters = rcu_state.sync_head;
    rcu_state.sync_head = 0;
    if (waiters) {
        rcu_sync_node_t *tail = waiters;
        while (tail->next) tail = tail->next;
        tail->next = rcu_state.sync_ready;
        rcu_state.sync_ready = waiters;
    }

    rcu_state.defer_pending = 1;

    /* Chain: start next GP if callbacks queued while GP was in flight. */
    int need_gp = 0;
    for (int i = 0; i < SMP_MAX_CORES; i++) {
        if (rcu_state.cpu[i].cb_next_head) {
            need_gp = 1;
            break;
        }
    }

    spin_unlock_irq(&rcu_state.gp_lock, flags);

    if (need_gp)
        rcu_gp_start();
}

/* ── Read-side critical section ────────────── */

__attribute__((hot))
void rcu_read_lock(void) {
    struct thread *t = thread_current();
    if (__builtin_expect(t != 0, 1))
        t->rcu_read_nesting++;
    hal_cpu_wmb();
}

static void rcu_read_unlock_special(struct thread *t) {
    rcu_node_t *rnp = (rcu_node_t *)t->rcu_blocked_node;

    uint64_t flags;
    spin_lock_irq(&rnp->lock, &flags);
    blkd_remove(rnp, t);
    int can_complete = RCU_SEQ_ACTIVE(rnp->gp_seq) &&
                       rnp->qsmask == 0 &&
                       rnp->blkd_count == 0;
    spin_unlock_irq(&rnp->lock, flags);

    if (can_complete)
        rcu_gp_complete();
}

__attribute__((hot))
void rcu_read_unlock(void) {
    hal_cpu_wmb();
    struct thread *t = thread_current();
    if (__builtin_expect(t == 0, 0))
        return;
    int nesting = --t->rcu_read_nesting;
    if (__builtin_expect(nesting == 0, 1)) {
        if (__builtin_expect(t->rcu_blocked_node != 0, 0))
            rcu_read_unlock_special(t);
    }
}

/* ── Scheduler integration ─────────────────── */

__attribute__((hot))
void rcu_note_context_switch(struct thread *prev) {
    if (!prev)
        return;

    rcu_node_t *rnp = &rcu_state.node;
    int cpu_id = rcu_cpu_of_self();

    if (prev->rcu_read_nesting > 0) {
        /* Preempted inside read-side CS → blocked reader list */
        if (!prev->rcu_blocked_node) {
            uint64_t flags;
            spin_lock_irq(&rnp->lock, &flags);
            blkd_append(rnp, prev);
            spin_unlock_irq(&rnp->lock, flags);
        }
        return;
    }

    /* Not in read-side CS → this CPU passed a quiescent state */
    rcu_data_t *rdp = &rcu_state.cpu[cpu_id];
    if (!rdp->qs_pending)
        return;

    rdp->qs_pending = 0;

    uint64_t flags;
    spin_lock_irq(&rnp->lock, &flags);
    if (rnp->qsmask & (1u << cpu_id)) {
        rnp->qsmask &= ~(1u << cpu_id);
        int can_complete = (rnp->qsmask == 0 && rnp->blkd_count == 0);
        spin_unlock_irq(&rnp->lock, flags);
        if (can_complete)
            rcu_gp_complete();
    } else {
        spin_unlock_irq(&rnp->lock, flags);
    }
}

__attribute__((hot))
void rcu_check_callbacks(void) {
    int cpu_id = rcu_cpu_of_self();
    rcu_data_t *rdp = &rcu_state.cpu[cpu_id];

    if ((rdp->cb_next_head || rdp->cb_wait_head) &&
        RCU_SEQ_IDLE(READ_ONCE(rcu_state.gp_seq)))
        rcu_gp_start();
}

/* ── call_rcu ──────────────────────────────── */

void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *)) {
    head->func = func;
    head->next = 0;

    int cpu_id = rcu_cpu_of_self();
    rcu_data_t *rdp = &rcu_state.cpu[cpu_id];

    uint64_t flags = irq_save();
    if (rdp->cb_next_tail)
        rdp->cb_next_tail->next = head;
    else
        rdp->cb_next_head = head;
    rdp->cb_next_tail = head;
    rdp->cb_count++;
    irq_restore(flags);

    if (RCU_SEQ_IDLE(READ_ONCE(rcu_state.gp_seq)))
        rcu_gp_start();
}

/* ── synchronize_rcu ───────────────────────── */

void synchronize_rcu(void) {
    struct thread *self = thread_current();
    if (!self) return;

    /* Stack-allocated waiter node — lives until we're woken */
    rcu_sync_node_t node;
    node.waiter = self;

    uint64_t flags;
    spin_lock_irq(&rcu_state.gp_lock, &flags);
    node.next = rcu_state.sync_head;
    rcu_state.sync_head = &node;
    int need_start = RCU_SEQ_IDLE(rcu_state.gp_seq);
    spin_unlock_irq(&rcu_state.gp_lock, flags);

    if (need_start)
        rcu_gp_start();

    save_user_state_for_block(self, 0);
    self->state = THREAD_BLOCKED;
    schedule();
}

/* ── Init (cold, at bottom per convention) ─── */

__attribute__((cold))
void rcu_init(void) {
    rcu_state.gp_seq = 0;
    rcu_state.gp_lock = (spinlock_t)SPINLOCK_INIT;
    rcu_state.sync_head = 0;
    rcu_state.sync_ready = 0;
    rcu_state.defer_pending = 0;

    rcu_node_t *rnp = &rcu_state.node;
    rnp->lock = (spinlock_t)SPINLOCK_INIT;
    rnp->gp_seq = 0;
    rnp->qsmask = 0;
    rnp->qsmaskinit = 0;
    rnp->blkd_head = 0;
    rnp->blkd_tail = 0;
    rnp->blkd_count = 0;

    for (int i = 0; i < SMP_MAX_CORES; i++) {
        rnp->qsmaskinit |= (1u << i);
        rcu_data_t *rdp = &rcu_state.cpu[i];
        rdp->gp_seq = 0;
        rdp->qs_pending = 0;
        rdp->cb_wait_head = 0;
        rdp->cb_wait_tail = 0;
        rdp->cb_next_head = 0;
        rdp->cb_next_tail = 0;
        rdp->cb_done_head = 0;
        rdp->cb_done_tail = 0;
        rdp->cb_count = 0;
    }

    tick_register(&rcu_tick_cb, rcu_tick_deferred, TICK_EVERY);

    serial_puts("rcu: preemptible RCU init (");
    char t[4]; int ti = 0, v = SMP_MAX_CORES;
    do { t[ti++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" CPUs)\n");
}
