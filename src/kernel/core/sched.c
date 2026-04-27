/* CosmoRT Scheduler — Single-core, priority queues, one context_switch
 *
 * 32 priority levels (0 = SCHED_OTHER, 1-31 = RT).
 * Per-priority FIFO queues. Highest non-empty queue runs first.
 * SCHED_FIFO: no timeslice, runs until yield/block.
 * SCHED_RR: timeslice, rotates within same priority.
 * SCHED_OTHER: round-robin at priority 0.
 *
 * ONE switch mechanism: context_switch(prev, next) in context.asm.
 * schedule() is the SOLE entry point for all context switches.
 */

#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/frame.h"
#include "core/rcu.h"
#include "core/waitqueue.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "config.h"
#include "core/timer.h"
#include "hal/hal.h"

_Static_assert(__builtin_offsetof(thread_t, kstack_rsp) == 240,
               "kstack_rsp offset mismatch with context.asm THREAD_KSTACK_RSP");

/* Assembly: saves prev callee-saved+RFLAGS+RSP, loads next's */
extern void context_switch(thread_t *prev, thread_t *next);

/* Reschedule-Kern — deliver/ack primitives */
extern void check_pending_signals(void);
extern void lapic_eoi(void);

/* Run queue: single-core, protected by spinlock (IRQ context) */
static struct {
    thread_t *head;
    thread_t *tail;
} rq[PRIO_LEVELS];

static spinlock_t rq_lock = SPINLOCK_INIT;
static uint32_t rq_bitmap;  /* bit N set = rq[N] non-empty */

/* Idle thread: kstack_rsp used for context_switch, runs on boot stack */
static thread_t idle_thread;
static uint8_t idle_stack[16384] __attribute__((aligned(16)));

/* ── Enqueue / dequeue ───────────────────────────── */

__attribute__((hot))
void sched_add(thread_t *t) {
    int prio = t->priority;
    if (prio < 0) prio = 0;
    if (prio >= PRIO_LEVELS) prio = PRIO_LEVELS - 1;

    t->state = THREAD_RUNNABLE;

    uint64_t flags;
    spin_lock_irq(&rq_lock, &flags);

    /* Idempotency: walk the priority queue to detect membership.
     *
     * The earlier rq_next-based check was unsound: t->rq_next can be a stale
     * non-NULL pointer pointing into a freed slab slot if t was once enqueued
     * after some thread Y, Y was later picked (clears Y->rq_next but leaves
     * t->rq_next == &Y), Y was thread_free'd, and t was then dequeued via
     * sched_pick which only clears t->rq_next on the picked thread, not on
     * predecessors that already point past the (now reused) slot. After such
     * a sequence t->rq_next is non-NULL but t is not on any queue, so the
     * old check would refuse to enqueue and the wakeup would be lost.
     *
     * Walking the singly-linked list is O(n_in_queue); typical depth is
     * single-digit and the walk happens behind rq_lock, so this is cheap.
     * In return we get a structurally correct membership predicate. */
    for (thread_t *cur = rq[prio].head; cur; cur = cur->rq_next) {
        if (cur == t) {
            spin_unlock_irq(&rq_lock, flags);
            return;
        }
    }

    t->rq_next = 0;

    if (rq[prio].tail) {
        rq[prio].tail->rq_next = t;
    } else {
        rq[prio].head = t;
    }
    rq[prio].tail = t;
    rq_bitmap |= (1u << prio);

    spin_unlock_irq(&rq_lock, flags);
}

/* Test helper: plant stale rq_next, sched_add, sched_pick, all under
 * rq_lock so a peer CPU can't race the picked-by-other path. Returns
 * 0 on success, -10 if pick != t, -11 if t->rq_next != 0 after pick. */
int sched_test_stale_rq_next(thread_t *t) {
    uint64_t flags;
    spin_lock_irq(&rq_lock, &flags);

    t->state = THREAD_RUNNABLE;
    t->rq_next = (thread_t *)(uintptr_t)0xdeadbeefULL;

    /* Inline sched_add: list-walk idempotency, then enqueue. */
    int prio = 0;
    int already = 0;
    for (thread_t *cur = rq[prio].head; cur; cur = cur->rq_next) {
        if (cur == t) { already = 1; break; }
    }
    if (!already) {
        t->rq_next = 0;
        if (rq[prio].tail) rq[prio].tail->rq_next = t;
        else rq[prio].head = t;
        rq[prio].tail = t;
        rq_bitmap |= (1u << prio);
    }

    /* Inline sched_pick: must return t. */
    thread_t *picked = 0;
    if (rq_bitmap) {
        int p = 31 - __builtin_clz(rq_bitmap);
        picked = rq[p].head;
        if (picked) {
            rq[p].head = picked->rq_next;
            if (!rq[p].head) { rq[p].tail = 0; rq_bitmap &= ~(1u << p); }
            picked->rq_next = 0;
        }
    }
    int rc = 0;
    if (picked != t) rc = -10;
    else if (t->rq_next != 0) rc = -11;

    spin_unlock_irq(&rq_lock, flags);
    return rc;
}

/* Test helper: run sched_add(a,b,c), optional sched_dequeue(b), then 3x
 * sched_pick — all under rq_lock so a peer CPU's schedule() can't pluck
 * synthetic test threads off the rq before we can observe them.
 * Returns b->rq_next (must be 0 after sched_dequeue). */
int sched_test_drain_3(thread_t *a, thread_t *b, thread_t *c,
                       thread_t **p1, thread_t **p2, thread_t **p3,
                       int dequeue_b) {
    uint64_t flags;
    spin_lock_irq(&rq_lock, &flags);

    a->state = THREAD_RUNNABLE; a->rq_next = 0;
    b->state = THREAD_RUNNABLE; b->rq_next = 0;
    c->state = THREAD_RUNNABLE; c->rq_next = 0;

    /* Inline sched_add for a/b/c — single rq_lock window. */
    int prio = 0;
    rq[prio].tail ? (rq[prio].tail->rq_next = a) : (rq[prio].head = a);
    rq[prio].tail = a;
    a->rq_next = b; rq[prio].tail = b;
    b->rq_next = c; rq[prio].tail = c;
    rq_bitmap |= (1u << prio);

    if (dequeue_b) {
        /* Inline sched_dequeue(b) — same lock window. */
        if (rq[prio].head == b) {
            rq[prio].head = b->rq_next;
            if (!rq[prio].head) { rq[prio].tail = 0; rq_bitmap &= ~(1u << prio); }
        } else {
            thread_t *prev = 0;
            for (thread_t *cur = rq[prio].head; cur; cur = cur->rq_next) {
                if (cur == b) break;
                prev = cur;
            }
            if (prev && prev->rq_next == b) {
                prev->rq_next = b->rq_next;
                if (rq[prio].tail == b) rq[prio].tail = prev;
            }
        }
        b->rq_next = 0;
    }
    int b_rq_next_after = (b->rq_next != 0);

    /* Inline sched_pick — head of highest-prio queue. */
    thread_t *picks[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        thread_t *t = 0;
        if (rq_bitmap) {
            int p = 31 - __builtin_clz(rq_bitmap);
            t = rq[p].head;
            if (t) {
                rq[p].head = t->rq_next;
                if (!rq[p].head) { rq[p].tail = 0; rq_bitmap &= ~(1u << p); }
                t->rq_next = 0;
            }
        }
        picks[i] = t;
    }

    spin_unlock_irq(&rq_lock, flags);

    if (p1) *p1 = picks[0];
    if (p2) *p2 = picks[1];
    if (p3) *p3 = picks[2];
    return b_rq_next_after;
}

/* Remove t from its priority run queue if linked.
 * Required before thread_free: the singly-linked rq_next pointers in other
 * threads would otherwise dangle into a freed slab slot, breaking sched_pick
 * once the slot gets reused. Caller must own a reference on t (no concurrent
 * sched_pick can race because t->state != RUNNING is a precondition for
 * thread_free). */
void sched_dequeue(thread_t *t) {
    if (!t) return;

    int prio = t->priority;
    if (prio < 0) prio = 0;
    if (prio >= PRIO_LEVELS) prio = PRIO_LEVELS - 1;

    uint64_t flags;
    spin_lock_irq(&rq_lock, &flags);

    if (rq[prio].head == t) {
        rq[prio].head = t->rq_next;
        if (!rq[prio].head) {
            rq[prio].tail = 0;
            rq_bitmap &= ~(1u << prio);
        }
    } else {
        thread_t *prev = 0;
        for (thread_t *cur = rq[prio].head; cur; cur = cur->rq_next) {
            if (cur == t) { break; }
            prev = cur;
        }
        if (prev && prev->rq_next == t) {
            prev->rq_next = t->rq_next;
            if (rq[prio].tail == t) rq[prio].tail = prev;
        }
    }

    t->rq_next = 0;
    spin_unlock_irq(&rq_lock, flags);
}

/* sched_wake — Wrapper auf try_to_wake_up(t, TASK_NORMAL).
 *
 * Wirkung unveraendert gegenueber dem alten direkten CAS-Pfad: TASK_NORMAL
 * deckt THREAD_BLOCKED + THREAD_STOPPED, beides via einen einzigen state-CAS
 * gewickelt in waitqueue.c::try_to_wake_up. Damit existiert genau *eine*
 * Wake-Primitive im Kernel, statt der alten Doppelimplementierung. */
void sched_wake(thread_t *t) {
    try_to_wake_up(t, TASK_NORMAL);
}

__attribute__((hot))
thread_t *sched_pick(void) {
    uint64_t flags;
    spin_lock_irq(&rq_lock, &flags);

    thread_t *t = 0;

    if (rq_bitmap) {
        int prio = 31 - __builtin_clz(rq_bitmap);

        t = rq[prio].head;
        if (t) {
            rq[prio].head = t->rq_next;
            if (!rq[prio].head) {
                rq[prio].tail = 0;
                rq_bitmap &= ~(1u << prio);
            }
            t->rq_next = 0;
        }
    }

    spin_unlock_irq(&rq_lock, flags);
    return t;
}

/* ── finish_switch — enqueue prev AFTER context_switch ─
 * Linux: finish_task_switch(). Runs on next's stack, prev's stack is free.
 * Called from schedule() after context_switch, and from ret_from_fork. */

void finish_switch(void) {
    percpu_t *cpu = percpu_self();
    thread_t *prev = cpu->switch_prev;
    cpu->switch_prev = 0;
    if (prev && prev != &idle_thread && prev->state == THREAD_RUNNING)
        sched_add(prev);
}

/* ── schedule() — THE one switch point ──────────── */

extern void tss_set_rsp0(uint64_t rsp0);
extern uint64_t pml4[];

void schedule(void) {
    percpu_t *cpu = percpu_self();
    thread_t *prev = cpu->current_thread;

    /* Pick next runnable thread, skip dead/invalid.
     * prev is NOT enqueued yet — finish_switch does it after the switch. */
    thread_t *next;
    for (;;) {
        next = sched_pick();
        if (!next) {
            if (prev && prev != &idle_thread && prev->state == THREAD_RUNNING)
                return;
            next = &idle_thread;
            break;
        }
        if (next->state == THREAD_DEAD || next->state == THREAD_FREE ||
            next->state == THREAD_STOPPED ||
            !next->proc || !next->proc->pml4 ||
            next->proc->state != PROC_ALIVE) {
            if (next->state == THREAD_DEAD && !next->proc)
                thread_free(next);
            continue;
        }
        break;
    }

    if (prev == next) {
        if (next != &idle_thread) next->state = THREAD_RUNNING;
        return;
    }

    /* Save prev per-thread state (skip idle and dead threads).
     *
     * GS-base note: KERNEL_GS_BASE holds the user GS while CPU is in kernel
     * mode (swapped in by syscall/IRQ entry's swapgs). On x86_64 the same
     * MSR is also primed at boot to the per-CPU base for the *first* swapgs.
     * After that, only ARCH_SET_GS or wrgsbase from userspace ever changes
     * it. CosmoRT today: zero callers use GS for user TLS, so we keep the
     * MSR untouched in the hot context-switch path and update it only when
     * a thread has explicitly opted in (gs_base != 0). This preserves the
     * percpu-base invariant for fresh threads (gs_base == 0) without losing
     * round-trip semantics for any thread that sets a real GS. */
    if (prev && prev != &idle_thread && prev->state != THREAD_DEAD) {
        prev->saved_user_rsp = cpu->user_rsp;
        prev->fs_base = hal_cpu_get_tls();
        if (prev->gs_base) prev->gs_base = hal_cpu_get_user_gs();
        hal_cpu_fpu_save(prev->xsave_area);
    }

    /* Load next state */
    if (next != &idle_thread) {
        next->state = THREAD_RUNNING;
        hal_mmu_switch(virt_to_phys(next->proc->pml4));
        tss_set_rsp0(next->kstack_top);
        cpu->kernel_rsp = next->kstack_top;
        cpu->user_rsp = next->saved_user_rsp;
        cpu->syscall_frame = next->kstack_top - 15 * 8;
        hal_cpu_set_tls(next->fs_base);
        if (next->gs_base) hal_cpu_set_user_gs(next->gs_base);
        hal_cpu_fpu_restore(next->xsave_area);
    } else {
        uint64_t idle_top = (uint64_t)(uintptr_t)(idle_stack + sizeof(idle_stack));
        hal_mmu_switch(virt_to_phys(pml4));
        tss_set_rsp0(idle_top);
        cpu->kernel_rsp = idle_top;
    }

    /* RCU quiescent state: prev is leaving the CPU.
     * If prev has rcu_read_nesting > 0, it's a preempted reader → blocked list.
     * If prev has rcu_read_nesting == 0, this CPU passed a quiescent state. */
    rcu_note_context_switch(prev);

    cpu->switch_prev = prev;
    cpu->current_thread = next;
    context_switch(prev, next);
    /* Now on next's stack. Enqueue prev (SMP-safe: prev's stack is free). */
    finish_switch();
}

/* ── Timer preemption ────────────────────────────── */

void sched_preempt(irq_frame_t *f) {
    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || cur == &idle_thread || cur->state != THREAD_RUNNING) return;

    /* Kernel mode — don't preempt, IRQ interrupted kernel code path. */
    if (!frame_is_user(f)) return;

    /* Signal delivery (without context switch) */
    {
        process_t *p = cur->proc;
        uint64_t deliverable = p ? ((p->sig_pending | cur->sig_thread_pending) & ~cur->sig_blocked) : 0;
        int alarm_due = p && p->alarm_deadline_ms > 0 && timer_ms() >= p->alarm_deadline_ms;
        if (deliverable || alarm_due) {
            cpu->in_preempt = 1;
            irq_frame_to_thread(f, cur);
            /* Snapshot FS_BASE so deliver_signal's sigframe captures the
             * right TLS pointer. Not restored here: deliver_signal may set
             * a fresh FS_BASE via sigframe, sigreturn restores original.
             * GS-base: skip — KERNEL_GS_BASE shares boot percpu init. */
            cur->fs_base = hal_cpu_get_tls();
            check_pending_signals();
            thread_to_irq_frame(cur, f);
            cpu->in_preempt = 0;
        }
    }

    /* RCU: check if grace period needs advancing */
    rcu_check_callbacks();

    /* SCHED_FIFO: never preempt (runs until yield/block) */
    if (cur->sched_policy == SCHED_FIFO) return;

    /* SCHED_RR: decrement timeslice */
    if (cur->sched_policy == SCHED_RR) {
        if (cur->timeslice > 0) {
            cur->timeslice--;
            return;
        }
        cur->timeslice = RR_TIMESLICE;
    }

    /* Save current thread context from interrupt frame */
    irq_frame_to_thread(f, cur);
    cur->fs_base = hal_cpu_get_tls();

    /* cur->state stays THREAD_RUNNING — schedule() re-enqueues */
    lapic_eoi();

    schedule();

    /* Returned: we've been rescheduled.
     * schedule() already loaded our CR3, TSS, FPU, FS_BASE.
     * Restore user regs from thread_t to IRQ frame. */
    thread_to_irq_frame(cur, f);
}

/* ── Init + scheduler loop ───────────────────────── */

__attribute__((cold))
void sched_init(void) {
    for (int i = 0; i < PRIO_LEVELS; i++) {
        rq[i].head = rq[i].tail = 0;
    }
    rq_lock = (spinlock_t)SPINLOCK_INIT;
    rq_bitmap = 0;
    serial_puts("sched: single-core init (");
    char t[4]; int ti = 0, v = PRIO_LEVELS;
    do { t[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" prio levels)\n");
}

void sched_loop(void) {
    percpu_t *cpu = percpu_self();
    cpu->current_thread = &idle_thread;
    idle_thread.state = THREAD_RUNNING;

    for (;;) {
        schedule();

        /* Returned: no runnable threads (or switched back to idle) */
        idle_thread.state = THREAD_RUNNING;
        cpu->current_thread = &idle_thread;

        hal_cpu_halt();
        hal_cpu_cli();
    }
}
