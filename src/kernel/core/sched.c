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
#include "hw/serial.h"
#include "spinlock.h"
#include "config.h"
#include "core/timer.h"
#include "arch/arch.h"

_Static_assert(__builtin_offsetof(thread_t, kstack_rsp) == 232,
               "kstack_rsp offset mismatch with context.asm THREAD_KSTACK_RSP");

/* Assembly: saves prev callee-saved+RFLAGS+RSP, loads next's */
extern void context_switch(thread_t *prev, thread_t *next);

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
    t->rq_next = 0;

    uint64_t flags;
    spin_lock_irq(&rq_lock, &flags);

    if (rq[prio].tail) {
        rq[prio].tail->rq_next = t;
    } else {
        rq[prio].head = t;
    }
    rq[prio].tail = t;
    rq_bitmap |= (1u << prio);

    spin_unlock_irq(&rq_lock, flags);
}

void sched_wake(thread_t *t) {
    if (!t) return;
    int old = __sync_val_compare_and_swap(&t->state, THREAD_BLOCKED, THREAD_RUNNABLE);
    if (old == THREAD_BLOCKED) { sched_add(t); return; }
    /* Also wake STOPPED threads (SIGCONT) */
    old = __sync_val_compare_and_swap(&t->state, THREAD_STOPPED, THREAD_RUNNABLE);
    if (old == THREAD_STOPPED) sched_add(t);
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

/* ── schedule() — THE one switch point ──────────── */

extern void tss_set_rsp0(uint64_t rsp0);
extern uint64_t pml4[];

void schedule(void) {
    percpu_t *cpu = percpu_self();
    thread_t *prev = cpu->current_thread;

    /* Re-enqueue prev if still RUNNING (preemption / yield) */
    if (prev && prev != &idle_thread && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_RUNNABLE;
        sched_add(prev);
    }

    /* Pick next runnable thread, skip dead/invalid */
    thread_t *next;
    for (;;) {
        next = sched_pick();
        if (!next) { next = &idle_thread; break; }
        if (next->state == THREAD_DEAD || next->state == THREAD_FREE ||
            !next->proc || !next->proc->pml4) {
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

    /* Save prev per-thread state (skip idle and dead threads) */
    if (prev && prev != &idle_thread && prev->state != THREAD_DEAD) {
        prev->saved_user_rsp = cpu->user_rsp;
        prev->fs_base = arch_get_fs_base();
        arch_fpstate_save(prev->xsave_area);
    }

    /* Load next state */
    if (next != &idle_thread) {
        next->state = THREAD_RUNNING;
        arch_set_cr3(virt_to_phys(next->proc->pml4));
        tss_set_rsp0(next->kstack_top);
        cpu->kernel_rsp = next->kstack_top;
        /* Restore per-CPU state that syscall_entry_asm writes per-thread:
         * user_rsp (sysret reads it), syscall_frame (signal delivery reads it).
         * For new threads (ret_from_fork), these are set by ret_from_fork itself. */
        cpu->user_rsp = next->saved_user_rsp;
        cpu->syscall_frame = next->kstack_top - 15 * 8;
        arch_set_fs_base(next->fs_base);
        arch_fpstate_restore(next->xsave_area);
    } else {
        uint64_t idle_top = (uint64_t)(uintptr_t)(idle_stack + sizeof(idle_stack));
        arch_set_cr3(virt_to_phys(pml4));
        tss_set_rsp0(idle_top);
        cpu->kernel_rsp = idle_top;
    }

    cpu->current_thread = next;
    context_switch(prev, next);
    /* Returns here when WE are switched back to */
}

/* ── Timer preemption ────────────────────────────── */

void sched_preempt(void *frame_ptr) {
    {
        extern void epoll_check_timeouts(void);
        epoll_check_timeouts();
    }

    /* Check alarm timers for the current thread's process */
    {
        thread_t *alarm_t = percpu_self()->current_thread;
        if (alarm_t && alarm_t != &idle_thread && alarm_t->proc) {
            process_t *ap = alarm_t->proc;
            if (ap->alarm_deadline_ms > 0 && timer_ms() >= ap->alarm_deadline_ms) {
                ap->alarm_deadline_ms = 0;
                void *handler = ap->sig_actions[SIGALRM].sa_handler;
                if ((uint64_t)handler > 1)
                    ap->sig_pending |= SIG_BIT(SIGALRM);
                else if (handler == (void *)0) {
                    ap->exit_signal = SIGALRM;
                    ap->sig_pending |= SIG_BIT(SIGALRM);
                }
            }
        }
    }
    /* Scan ALL processes for alarms (catches blocked threads) */
    {
        extern void check_alarm_timers(void);
        check_alarm_timers();
    }

    /* VT flush */
    {
        extern void vt_flush(int vt_id);
        extern int vt_active(void);
        vt_flush(vt_active());
    }

    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || cur == &idle_thread || cur->state != THREAD_RUNNING) return;

    uint64_t *f = (uint64_t *)frame_ptr;

    if ((f[18] & 3) != 3) return; /* kernel mode — don't preempt */

    /* Signal delivery (without context switch) */
    extern void check_pending_signals(void);
    {
        process_t *p = cur->proc;
        uint64_t deliverable = p ? ((p->sig_pending | cur->sig_thread_pending) & ~cur->sig_blocked) : 0;
        int alarm_due = p && p->alarm_deadline_ms > 0 && timer_ms() >= p->alarm_deadline_ms;
        if (deliverable || alarm_due) {
            cpu->in_preempt = 1;
            cur->r15 = f[0]; cur->r14 = f[1]; cur->r13 = f[2]; cur->r12 = f[3];
            cur->r11 = f[4]; cur->r10 = f[5]; cur->r9 = f[6];  cur->r8 = f[7];
            cur->rbp = f[8]; cur->rdi = f[9]; cur->rsi = f[10]; cur->rdx = f[11];
            cur->rcx = f[12]; cur->rbx = f[13]; cur->rax = f[14];
            cur->rip = f[17]; cur->rflags = f[19]; cur->rsp = f[20];
            cur->fs_base = arch_get_fs_base();
            check_pending_signals();
            f[0] = cur->r15; f[1] = cur->r14; f[2] = cur->r13; f[3] = cur->r12;
            f[4] = cur->r11; f[5] = cur->r10; f[6] = cur->r9;  f[7] = cur->r8;
            f[8] = cur->rbp; f[9] = cur->rdi; f[10] = cur->rsi; f[11] = cur->rdx;
            f[12] = cur->rcx; f[13] = cur->rbx; f[14] = cur->rax;
            f[17] = cur->rip; f[19] = cur->rflags; f[20] = cur->rsp;
            cpu->in_preempt = 0;
        }
    }

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
    cur->r15 = f[0]; cur->r14 = f[1]; cur->r13 = f[2]; cur->r12 = f[3];
    cur->r11 = f[4]; cur->r10 = f[5]; cur->r9 = f[6];  cur->r8 = f[7];
    cur->rbp = f[8]; cur->rdi = f[9]; cur->rsi = f[10]; cur->rdx = f[11];
    cur->rcx = f[12]; cur->rbx = f[13]; cur->rax = f[14];
    cur->rip = f[17]; cur->rflags = f[19]; cur->rsp = f[20];
    cur->fs_base = arch_get_fs_base();

    /* cur->state stays THREAD_RUNNING — schedule() re-enqueues */
    extern void lapic_eoi(void);
    lapic_eoi();

    schedule();

    /* Returned: we've been rescheduled.
     * schedule() already loaded our CR3, TSS, FPU, FS_BASE.
     * Restore user regs from thread_t to IRQ frame. */
    f[0] = cur->r15; f[1] = cur->r14; f[2] = cur->r13; f[3] = cur->r12;
    f[4] = cur->r11; f[5] = cur->r10; f[6] = cur->r9;  f[7] = cur->r8;
    f[8] = cur->rbp; f[9] = cur->rdi; f[10] = cur->rsi; f[11] = cur->rdx;
    f[12] = cur->rcx; f[13] = cur->rbx; f[14] = cur->rax;
    f[17] = cur->rip; f[19] = cur->rflags; f[20] = cur->rsp;
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

        {
            extern void epoll_check_timeouts(void);
            epoll_check_timeouts();
        }
        arch_sti();
        arch_halt();
        arch_cli();
    }
}
