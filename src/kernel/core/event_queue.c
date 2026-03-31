/* CosmoRT Event Queue — kernel-side event_post + event_wait
 *
 * event_post: write event into target thread's queue, then sched_wake.
 * event_wait: consume next event; if empty, block thread (syscall restart).
 *
 * Race-free: event is in the queue BEFORE sched_wake. If the target
 * thread isn't blocked yet, it finds the event on next event_wait.
 * If already blocked, sched_wake wakes it and the re-executed syscall
 * calls event_wait which finds the event immediately.
 */

#include "core/event_queue.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/timer.h"
#include "core/rt.h"
#include "arch/arch.h"
#include "spinlock.h"

/* Per-thread producer lock — indexed by TID.
 * Protects head writes for multi-producer safety (IRQ + syscall). */
#define EQ_LOCK_MAX 512
static spinlock_t eq_locks[EQ_LOCK_MAX] = { [0 ... EQ_LOCK_MAX-1] = {0, 0} };

static inline spinlock_t *eq_lock_for(thread_t *t) {
    int idx = t->tid;
    if (idx < 0 || idx >= EQ_LOCK_MAX) idx = 0;
    return &eq_locks[idx];
}

/* ── Post: write event + wake target ─────────── */

void event_post(thread_t *target, uint32_t type, uint64_t data) {
    if (!target) return;
    event_queue_t *eq = &target->eq;

    spinlock_t *lk = eq_lock_for(target);
    uint64_t irqf;
    spin_lock_irq(lk, &irqf);

    uint32_t h = eq->head;
    uint32_t t = eq->tail;

    /* Queue full: advance tail (drop oldest) */
    if (h - t >= EQ_MAX_EVENTS)
        arch_store_release(&eq->tail, t + 1);

    /* Write event at head slot */
    eq->events[h & EQ_MASK].type = type;
    eq->events[h & EQ_MASK].data = data;
    arch_store_release(&eq->head, h + 1);

    spin_unlock_irq(lk, irqf);

    /* Wake target — sched_wake is atomic CAS (BLOCKED->RUNNABLE).
     * If target isn't blocked yet, this is a no-op. The event is
     * already in the queue, so next event_wait will find it. */
    sched_wake(target);
}

/* ── Wait: consume next event, block if empty ── */

extern uint64_t pml4[];
extern void save_user_state_for_block(thread_t *t, long return_value);
extern void thread_return_to_kernel(thread_t *t);
extern void epoll_sleeper_add_ext(thread_t *t);

/* Syscall saved frame layout — must match syscall_entry.asm push order */
typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t rax;       /* syscall number */
    uint64_t r11;       /* user RFLAGS */
    uint64_t rcx;       /* user RIP */
} eq_syscall_frame_t;

/* ── Block: pure timeout sleep (no event queue) ── */

void thread_block_ms(int timeout_ms) {
    if (timeout_ms <= 0) return;

    thread_t *cur = thread_current();
    if (!cur) return;

    percpu_t *cpu = percpu_self();
    eq_syscall_frame_t *frame = (eq_syscall_frame_t *)cpu->syscall_frame;
    uint64_t orig_syscall_nr = frame->rax;

    save_user_state_for_block(cur, 0);
    cur->rip -= 2;           /* back to `syscall` instruction (0F 05) */
    cur->rax = orig_syscall_nr;

    /* Check for deliverable signals — restart syscall immediately
     * so caller (e.g. do_nanosleep) sees the signal and returns -EINTR. */
    if (cur->proc) {
        uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
        uint64_t deliverable = all_pending & ~cur->sig_blocked;
        if (deliverable) {
            cur->state = THREAD_RUNNABLE;
            extern void sched_add(thread_t *t);
            sched_add(cur);
            arch_set_cr3(virt_to_phys(pml4));
            thread_return_to_kernel(cur);
            /* unreachable */
        }
    }

    cur->wake_at = timer_ms() + (uint64_t)timeout_ms;
    cur->wake_at_tsc = timer_deadline_tsc((uint64_t)timeout_ms);
    epoll_sleeper_add_ext(cur);
    cur->state = THREAD_BLOCKED;

    arch_set_cr3(virt_to_phys(pml4));
    thread_return_to_kernel(cur);
    /* unreachable */
}

int event_wait(event_queue_t *eq, event_t *out, int timeout_ms) {
    /* Fast path: event available */
    uint32_t h = arch_load_acquire(&eq->head);
    uint32_t t = eq->tail;  /* only consumer reads tail */

    if (h != t) {
        *out = eq->events[t & EQ_MASK];
        arch_store_release(&eq->tail, t + 1);
        return 0;
    }

    /* Queue empty — non-blocking mode */
    if (timeout_ms == 0)
        return -11; /* EAGAIN */

    /* Check for any deliverable signals before blocking.
     * Linux: any unblocked pending signal interrupts blocking syscalls
     * with -EINTR (or -ERESTARTSYS for SA_RESTART).
     * The signal is delivered by check_pending_signals on syscall return. */
    thread_t *cur = thread_current();
    if (!cur) return -14; /* EFAULT */
    if (cur->proc) {
        uint64_t all_pending = cur->proc->sig_pending | cur->sig_thread_pending;
        uint64_t deliverable = all_pending & ~cur->sig_blocked;
        if (deliverable) return -4; /* EINTR */
    }

    /* Block: save user state for syscall restart.
     * On wake, the syscall re-executes from userspace.
     * event_wait is called again and finds the event in the queue. */

    /* Read original syscall number from frame before save overwrites rax */
    percpu_t *cpu = percpu_self();
    eq_syscall_frame_t *frame = (eq_syscall_frame_t *)cpu->syscall_frame;
    uint64_t orig_syscall_nr = frame->rax;

    save_user_state_for_block(cur, 0);
    cur->rip -= 2;           /* back to `syscall` instruction (0F 05) */
    cur->rax = orig_syscall_nr;

    /* Timeout — TSC-based for sub-ms precision */
    if (timeout_ms > 0) {
        cur->wake_at = timer_ms() + (uint64_t)timeout_ms;
        cur->wake_at_tsc = timer_deadline_tsc((uint64_t)timeout_ms);
    } else {
        cur->wake_at = 0;
        cur->wake_at_tsc = 0;
    }

    /* Register for timeout checking */
    epoll_sleeper_add_ext(cur);

    cur->state = THREAD_BLOCKED;

    /* Close race: event_post writes event then sched_wake(CAS BLOCKED→RUNNABLE).
     * If event arrived between our fast-path check and BLOCKED, sched_wake saw
     * RUNNING/... and was a no-op. Re-check after BLOCKED. mfence ensures our
     * BLOCKED store is globally visible before we read head (so event_post on
     * another core sees BLOCKED and its sched_wake succeeds). */
    __asm__ volatile("mfence" ::: "memory");
    if (arch_load_acquire(&eq->head) != eq->tail) {
        /* Event arrived — undo block, let scheduler re-run us */
        cur->state = THREAD_RUNNABLE;
        extern void sched_add(thread_t *t);
        sched_add(cur);
    }

    arch_set_cr3(virt_to_phys(pml4));
    thread_return_to_kernel(cur);
    return 0; /* unreachable */
}
