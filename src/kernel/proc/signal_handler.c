/* CosmoRT Syscall Layer — sigaction, sigaltstack, signal helpers */

#include "sys/internal.h"
#include "core/event_queue.h"
#include "core/tick.h"

/* sigaltstack flags */
#define SS_ONSTACK 1
#define SS_DISABLE 2

/* ── SYS_SIGALTSTACK (131) ───────────────────────── */

struct k_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
};

long do_sigaltstack(const void *ss_, void *oss_) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    /* Return old stack first */
    if (oss_) {
        struct k_stack_t koss;
        koss.ss_sp    = t->sigalt_sp;
        koss.ss_size  = t->sigalt_size;
        koss._pad     = 0;
        /* SS_ONSTACK if currently executing on the altstack */
        if (t->sigalt_sp && !(t->sigalt_flags & SS_DISABLE) &&
            t->rsp >= t->sigalt_sp && t->rsp < t->sigalt_sp + t->sigalt_size)
            koss.ss_flags = SS_ONSTACK;
        else if (t->sigalt_flags & SS_DISABLE)
            koss.ss_flags = SS_DISABLE;
        else
            koss.ss_flags = 0;
        int r = copy_to_user(oss_, &koss, sizeof(koss));
        if (r) return r;
    }

    if (ss_) {
        struct k_stack_t kss;
        int r = copy_from_user(&kss, ss_, sizeof(kss));
        if (r) return r;

        if (kss.ss_flags & ~SS_DISABLE) return -EINVAL; /* unknown flags */

        if (kss.ss_flags & SS_DISABLE) {
            t->sigalt_sp    = 0;
            t->sigalt_size  = 0;
            t->sigalt_flags = SS_DISABLE;
        } else {
            if (kss.ss_size < 2048) return -ENOMEM; /* MINSIGSTKSZ */
            t->sigalt_sp    = kss.ss_sp;
            t->sigalt_size  = kss.ss_size;
            t->sigalt_flags = 0;
        }
    }

    return 0;
}

/* ── Signals (2.4) ───────────────────────────────── */

/* SIG_DFL, SIG_IGN, SIGKILL, SIGSEGV etc. — from linux.h */

long do_rt_sigaction(int sig, const void *act_,
                            void *oldact_, size_t sigsetsize) {
    const struct k_sigaction *act = (const struct k_sigaction *)act_;
    struct k_sigaction *oldact = (struct k_sigaction *)oldact_;
    /* musl passes sigsetsize=8 (64 signals). Accept 8 or 16. */
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    if (sig < 1 || sig >= 64) return -EINVAL;
    if (sig == SIGKILL) return -EINVAL; /* SIGKILL cannot be caught */

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    if (oldact) {
        int r = copy_to_user(oldact, &p->sig_actions[sig], sizeof(struct k_sigaction));
        if (r) return r;
    }

    if (act) {
        struct k_sigaction k_act;
        int r = copy_from_user(&k_act, act, sizeof(k_act));
        if (r) return r;
        p->sig_actions[sig] = k_act;
    }

    return 0;
}

/* ── send_sigpipe — SIGPIPE for pipe/socket write on broken end ── */

long send_sigpipe(void) {
    process_t *p = proc_current();
    if (p) {
        void *handler = p->sig_actions[SIGPIPE].sa_handler;
        if (handler != SIG_IGN)
            p->sig_pending |= SIG_BIT(SIGPIPE);
    }
    return -EPIPE;
}

/* ── SYS_ALARM (37) ─────────────────────────────── */

long do_alarm(unsigned int seconds) {
    process_t *p = proc_current();
    if (!p) return 0;

    uint64_t now = timer_ms();
    unsigned int remaining = 0;

    /* Calculate remaining seconds from previous alarm */
    if (p->alarm_deadline_ms > 0 && p->alarm_deadline_ms > now)
        remaining = (unsigned int)((p->alarm_deadline_ms - now + 999) / 1000);

    if (seconds == 0) {
        /* Cancel */
        p->alarm_deadline_ms = 0;
    } else {
        p->alarm_deadline_ms = now + (uint64_t)seconds * 1000;
    }

    return (long)remaining;
}

/* Check all processes for expired alarm timers.
 * Registered in alarm_init() via tick_register. Runs every tick.
 * Per-Prozess-Scan O(pid_table_capacity) — Folge-Arbeit: Timer-Wheel. */
void check_alarm_timers(void) {
    uint64_t now = timer_ms();
    extern process_t **pid_table;
    int cap = pid_table_capacity();
    for (int i = 1; i < cap; i++) {
        process_t *p = pid_table[i];
        if (!p || p->state != PROC_ALIVE) continue;
        if (p->alarm_deadline_ms == 0 || now < p->alarm_deadline_ms) continue;
        p->alarm_deadline_ms = 0;
        /* Set SIGALRM pending and wake blocked threads.
         * Actual delivery happens in event_wait signal check or
         * check_signals_syscall_path on next syscall return. */
        p->sig_pending |= SIG_BIT(SIGALRM);
        extern void sched_wake(thread_t *t);
        thread_t *t = p->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED)
                sched_wake(t);
            t = t->proc_next;
        }
    }
}

static struct tick_callback alarm_cb;

/* RLIMIT_CPU enforcement — tick-granularity CPU accounting.
 * Timer fires at 1000Hz, so one tick ≈ 1ms of wall time. We charge the tick
 * to the currently-running non-idle thread on this CPU (single-core).
 * When accumulated CPU-seconds crosses a threshold:
 *   soft limit reached         → SIGXCPU once per second
 *   hard limit reached         → SIGKILL
 * Linux: both are subject to sig_actions (SIGXCPU default = terminate+core). */
#define TICKS_PER_SECOND 1000
static struct tick_callback cpu_limit_cb;

/* Runs in timer-IRQ context. Charges the current thread's tick to its process
 * and sets signal-pending bits when RLIMIT_CPU is exceeded. Does NOT invoke
 * kill_one() directly because that can call do_exit_group() for current — not
 * safe in IRQ context. Delivery happens on syscall-return / preempt path. */
static void check_cpu_limits(void) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu ? cpu->current_thread : 0;
    if (!t || !t->proc) return;
    process_t *p = t->proc;
    if (p->state != PROC_ALIVE) return;

    uint64_t ticks = ++p->cpu_time_ticks;
    unsigned long soft = p->rlim_cpu_soft;
    unsigned long hard = p->rlim_cpu_hard;
    if (!soft && !hard) return;

    uint64_t secs = ticks / TICKS_PER_SECOND;

    if (hard && secs >= hard) {
        /* SIGKILL cannot be blocked/ignored — delivery happens on return. */
        p->sig_pending |= SIG_BIT(SIGKILL);
        return;
    }
    if (soft && secs >= soft && secs > p->xcpu_last_sec) {
        p->xcpu_last_sec = secs;
        p->sig_pending |= SIG_BIT(SIGXCPU);
    }
}

void alarm_init(void) {
    tick_register(&alarm_cb, check_alarm_timers, TICK_EVERY);
    tick_register(&cpu_limit_cb, check_cpu_limits, TICK_EVERY);
}

/* Check and deliver signals in the SYSCALL return path.
 * Syncs percpu syscall frame <-> thread_t around delivery.
 * Called from syscall_entry.asm between sys_handler return and SYSRET.
 * Actually called from the ASM-adjacent C code. */
/* Is this syscall restartable when interrupted by a signal with SA_RESTART? */
static int is_restartable_syscall(long num) {
    return num == SYS_READ || num == SYS_WRITE || num == SYS_READV ||
           num == SYS_WRITEV || num == SYS_WAIT4 || num == SYS_NANOSLEEP ||
           num == SYS_CLOCK_NANOSLEEP || num == SYS_POLL ||
           num == SYS_RECVFROM || num == SYS_SENDTO ||
           num == SYS_RECVMSG || num == SYS_SENDMSG ||
           num == SYS_ACCEPT || num == SYS_ACCEPT4 ||
           num == SYS_CONNECT;
}

/* Nach einem handler-ret kommt SYS_RT_SIGRETURN. Wenn dnotify_fire mehrere
 * (fd, sig)-Tupel fuer dasselbe Signal queued hat, re-pended deliver_signal
 * sig_pending — aber der normale Early-Return haette diesen nested Handler
 * erst beim naechsten Syscall ausgeliefert. Linux stellt nested Signale
 * zwischen handler-ret und return-to-user als zweiten Frame zu.
 *
 * Gezielte Loesung: nested Delivery nur wenn die dnotify-Queue noch
 * Eintraege fuer ein aktuell pending Signal haelt. Andere pending Signale
 * (SIGCHLD im futex-Pfad, pthread_cancel, ...) bleiben fuer den naechsten
 * Syscall — der globale Check wurde 528d571/c3b602f als zu invasiv
 * revertiert (pthread_cond/capset04). */
static int dnotify_nested_pending(process_t *p, thread_t *t) {
    if (p->dnotify_q_head == p->dnotify_q_tail) return 0;
    uint64_t queue_mask = 0;
    int h = p->dnotify_q_head;
    while (h != p->dnotify_q_tail) {
        int s = p->dnotify_q_sig[h];
        if (s >= 1 && s < 64) queue_mask |= SIG_BIT(s);
        h = (h + 1) & 15;
    }
    return ((p->sig_pending & queue_mask) & ~t->sig_blocked) != 0;
}

void check_signals_syscall_path(long *result_ptr, long num) {
    if (num == SYS_RT_SIGRETURN) {
        thread_t *tn = thread_current();
        if (!tn || !tn->proc) return;
        if (!dnotify_nested_pending(tn->proc, tn)) return;
        /* Fall-Through in den gemeinsamen Delivery-Pfad unten — frame->rcx,
         * frame->r11, cpu->user_rsp wurden von do_rt_sigreturn schon mit
         * dem restaurierten ucontext ueberschrieben. */
    }
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;

    /* Check alarm before deliverable test — alarm may set a new pending bit */
    if (p->alarm_deadline_ms > 0 && timer_ms() >= p->alarm_deadline_ms) {
        p->alarm_deadline_ms = 0;
        void *handler = p->sig_actions[SIGALRM].sa_handler;
        if (handler == SIG_IGN) { /* ignore */ }
        else if (handler == SIG_DFL) {
            p->exit_signal = SIGALRM;
            do_exit(128 + SIGALRM);
            return;
        } else {
            p->sig_pending |= SIG_BIT(SIGALRM);
        }
    }

    uint64_t deliverable = (p->sig_pending | t->sig_thread_pending) & ~t->sig_blocked;
    if (!deliverable) return;

    percpu_t *cpu = percpu_self();
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    /* Save syscall frame -> thread_t */
    t->rip    = frame->rcx;
    t->rflags = frame->r11;
    t->rsp    = cpu->user_rsp;
    t->rax    = (uint64_t)*result_ptr;
    t->rbx = frame->rbx; t->rdx = frame->rdx;
    t->rsi = frame->rsi; t->rdi = frame->rdi; t->rbp = frame->rbp;
    t->r8  = frame->r8;  t->r9  = frame->r9;  t->r10 = frame->r10;
    t->r12 = frame->r12; t->r13 = frame->r13;
    t->r14 = frame->r14; t->r15 = frame->r15;
    /* Save live FS_BASE so deliver_signal stores correct TLS in ucontext */
    t->fs_base = hal_cpu_get_tls();

    /* SA_RESTART: if syscall returned -EINTR and the about-to-be-delivered
     * signal has SA_RESTART, set up registers so rt_sigreturn restarts the
     * syscall instead of returning -EINTR. */
    if (*result_ptr == -EINTR && is_restartable_syscall(num)) {
        /* Find which signal will be delivered */
        for (int sig = 1; sig < 64; sig++) {
            if (!(deliverable & SIG_BIT(sig))) continue;
            struct k_sigaction *sa = &p->sig_actions[sig];
            if ((uint64_t)sa->sa_handler > 1 && (sa->sa_flags & SA_RESTART)) {
                /* Rewind RIP to re-execute syscall instruction.
                 * Set RAX to syscall number so it re-enters correctly. */
                t->rip -= 2;        /* back over `syscall` (0F 05) */
                t->rax = (uint64_t)num;
            }
            break; /* only first deliverable signal matters */
        }
    }

    check_pending_signals();

    /* SIGSTOP: thread was stopped by check_pending_signals.
     * Yield to scheduler — on SIGCONT, syscall will restart. */
    if (t->state == THREAD_STOPPED) {
        extern void schedule(void);
        schedule();
        /* Returned: SIGCONT woke us. Restore thread_t → frame below. */
    }

    /* Write back thread_t -> syscall frame */
    frame->rcx = t->rip;
    frame->r11 = t->rflags;
    cpu->user_rsp = t->rsp;
    *result_ptr = (long)t->rax;
    frame->rbx = t->rbx; frame->rdx = t->rdx;
    frame->rsi = t->rsi; frame->rdi = t->rdi; frame->rbp = t->rbp;
    frame->r8  = t->r8;  frame->r9  = t->r9;  frame->r10 = t->r10;
    frame->r12 = t->r12; frame->r13 = t->r13;
    frame->r14 = t->r14; frame->r15 = t->r15;
}
