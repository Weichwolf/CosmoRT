/* CosmoRT Syscall Layer — sigaction, sigaltstack, signal helpers */

#include "internal.h"
#include "core/event_queue.h"

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
 * Called from timer tick on BSP. Sets SIGALRM pending + wakes blocked threads. */
void check_alarm_timers(void) {
    uint64_t now = timer_ms();
    extern process_t *pid_table[];
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *p = pid_table[i];
        if (!p || p->state != PROC_ALIVE) continue;
        if (p->alarm_deadline_ms == 0 || now < p->alarm_deadline_ms) continue;
        p->alarm_deadline_ms = 0;
        void *handler = p->sig_actions[SIGALRM].sa_handler;
        if (handler == SIG_IGN) continue;
        if (handler == SIG_DFL) {
            /* Default: terminate */
            p->exit_signal = SIGALRM;
            p->state = PROC_ZOMBIE;
            p->exit_code = 128 + SIGALRM;
            continue;
        }
        /* User handler: set pending + wake blocked threads */
        p->sig_pending |= SIG_BIT(SIGALRM);
        {
            extern void event_post(thread_t *target, uint32_t type, uint64_t data);
            thread_t *t = p->threads;
            while (t) {
                if (t->state == THREAD_BLOCKED)
                    event_post(t, 1 /* EQ_CHILD_EXITED */, (uint64_t)SIGALRM);
                t = t->proc_next;
            }
        }
    }
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

void check_signals_syscall_path(long *result_ptr, long num) {
    if (num == SYS_RT_SIGRETURN) return;
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
    t->fs_base = arch_get_fs_base();

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
        extern uint64_t pml4[];
        extern void thread_return_to_kernel(thread_t *t);
        /* RIP already points at user code; back up to re-execute syscall on resume */
        t->rip = frame->rcx;  /* original user RIP (pre-signal) */
        t->rsp = cpu->user_rsp;
        t->rax = frame->rax;  /* original syscall nr for restart */
        t->rip -= 2;          /* back to `syscall` instruction (0F 05) */
        arch_set_cr3(virt_to_phys(pml4));
        thread_return_to_kernel(t);
        /* unreachable */
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
