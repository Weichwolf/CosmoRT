/* CosmoRT Syscall Layer — signal delivery, kill, mask operations */

#include "internal.h"
#include "core/event_queue.h"

/* ── check_pending_signals — deliver pending signals to current thread ── */

/* Check and deliver pending signals. Operates on thread_t register fields.
 * Callers must sync hardware frame ↔ thread_t before/after. */
void check_pending_signals(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;

    /* Check per-process alarm timer */
    if (p->alarm_deadline_ms > 0 && timer_ms() >= p->alarm_deadline_ms) {
        p->alarm_deadline_ms = 0;
        /* Deliver SIGALRM like kill(getpid(), SIGALRM) */
        void *handler = p->sig_actions[SIGALRM].sa_handler;
        if (handler == SIG_IGN) { /* ignore */ }
        else if (handler == SIG_DFL) {
            p->exit_signal = SIGALRM;
            do_exit(128 + SIGALRM);
            return;
        } else {
            p->sig_pending |= (1ULL << SIGALRM);
        }
    }

    /* If returning from rt_sigsuspend, restore the original mask before
     * delivering the signal so the ucontext captures the pre-sigsuspend mask. */
    if (t->in_sigsuspend) {
        t->sig_blocked = t->sig_saved_mask;
        t->in_sigsuspend = 0;
    }

    uint64_t deliverable = p->sig_pending & ~t->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < 64; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;
        p->sig_pending &= ~(1ULL << sig);

        struct k_sigaction *sa = &p->sig_actions[sig];
        uint64_t handler = (uint64_t)sa->sa_handler;
        /* Debug: log signal delivery for pid=2 */
        if (p->pid == 2 && handler > 1) {
            serial_puts("SIG_DELIVER: sig=");
            serial_hex64((uint64_t)(unsigned)sig);
            serial_puts(" handler=");
            serial_hex64(handler);
            serial_puts(" rip=");
            serial_hex64(t->rip);
            serial_putchar('\n');
        }

        if (handler == 1) continue; /* SIG_IGN */

        if (handler == 0) {
            /* SIG_DFL — POSIX default actions */
            switch (sig) {
            /* Ignore */
            case 17: /* SIGCHLD */
            case 23: /* SIGURG */
            case 28: /* SIGWINCH */
            case 29: /* SIGIO */
                continue;
            /* Stop (SIGTSTP, SIGSTOP, SIGTTIN, SIGTTOU) — stop current thread */
            case 19: case 20: case 21: case 22: {
                /* Stop all threads in process */
                thread_t *th = p->threads;
                while (th) {
                    if (th->state == THREAD_RUNNING || th->state == THREAD_RUNNABLE)
                        th->state = THREAD_STOPPED;
                    th = th->proc_next;
                }
                /* Record stop signal for wait4(WUNTRACED) */
                p->stop_signal = sig;
                p->was_continued = 0;
                /* Notify parent (wake if blocked in wait4) */
                if (p->parent_pid) {
                    process_t *parent = proc_find(p->parent_pid);
                    if (parent) {
                        thread_t *pt = parent->threads;
                        while (pt) {
                            event_post(pt, EQ_CHILD_STOPPED, ((uint64_t)sig << 32) | p->pid);
                            pt = pt->proc_next;
                        }
                    }
                }
                /* Current thread stopped — save state and yield to scheduler */
                {
                    extern void save_user_state_for_block(thread_t *t, long return_value);
                    extern void thread_return_to_kernel(thread_t *t);
                    extern uint64_t pml4[];
                    percpu_t *cpu = percpu_self();
                    typedef struct {
                        uint64_t r15, r14, r13, r12, rbp, rbx;
                        uint64_t r9, r8, r10, rdx, rsi, rdi;
                        uint64_t rax; uint64_t r11; uint64_t rcx;
                    } sf_t;
                    sf_t *frame = (sf_t *)cpu->syscall_frame;
                    uint64_t orig_nr = frame->rax;
                    save_user_state_for_block(t, 0);
                    t->rip -= 2; /* back to syscall instruction */
                    t->rax = orig_nr;
                    t->state = THREAD_STOPPED;
                    arch_set_cr3(virt_to_phys(pml4));
                    thread_return_to_kernel(t);
                }
                return;
            }
            /* Continue (SIGCONT) — resume stopped threads */
            case 18: {
                extern void sched_add(thread_t *t);
                thread_t *th = p->threads;
                int resumed = 0;
                while (th) {
                    if (th->state == THREAD_STOPPED) {
                        th->state = THREAD_RUNNABLE;
                        sched_add(th);
                        resumed = 1;
                    }
                    th = th->proc_next;
                }
                if (resumed) {
                    p->stop_signal = 0;
                    p->was_continued = 1;
                }
                if (resumed && p->parent_pid) {
                    process_t *parent = proc_find(p->parent_pid);
                    if (parent) {
                        thread_t *pt = parent->threads;
                        while (pt) {
                            event_post(pt, EQ_CHILD_CONTINUED, (uint64_t)p->pid);
                            pt = pt->proc_next;
                        }
                    }
                }
                continue;
            }
            /* Terminate: everything else with SIG_DFL */
            default:
                p->exit_signal = sig;
                do_exit(128 + sig); /* doesn't return */
            }
        }

        /* User handler — deliver via thread_t modification */
        deliver_signal(t, sig);
        return; /* deliver one signal at a time */
    }
}

/* ── SYS_RT_SIGPROCMASK (14) ────────────────────────── */

long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                              size_t sigsetsize) {
    /* musl passes _NSIG/8 = 128/8 = 16 bytes (128 signals).
     * We support 64 signals (8 bytes). Accept 8 or 16, ignore upper bytes. */
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    if (oldset) {
        uint64_t tmp = t->sig_blocked;
        int r = copy_to_user(oldset, &tmp, 8);
        if (r) return r;
        /* Zero upper bytes if sigsetsize == 16 */
        if (sigsetsize == 16) {
            uint64_t zero = 0;
            r = copy_to_user(oldset + 1, &zero, 8);
            if (r) return r;
        }
    }

    if (set) {
        uint64_t k_set;
        int r = copy_from_user(&k_set, set, 8); /* only first 8 bytes matter */
        if (r) return r;
        uint64_t mask = k_set;
        mask &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL, SIGSTOP cannot be blocked */
        switch (how) {
        case 0: t->sig_blocked |= mask; break;  /* SIG_BLOCK */
        case 1: t->sig_blocked &= ~mask; break; /* SIG_UNBLOCK */
        case 2: t->sig_blocked = mask; break;    /* SIG_SETMASK */
        default: return -EINVAL;
        }
    }

    return 0;
}

/* ── SYS_KILL (62) ──────────────────────────────────── */

/* Send signal to a single process. Returns 0 or negative errno. */
static long kill_one(process_t *target, int sig) {
    /* Can't signal zombie/dead processes */
    if (target->state != PROC_ALIVE) return -ESRCH;

    /* Check handler */
    void *handler = target->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    /* SIG_DFL: kill the process for fatal signals */
    if (handler == SIG_DFL) {
        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, SIGIO */
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) return 0;
        /* Stop: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
            /* Set pending for delivery in check_pending_signals (process context).
             * For remote stop, set pending + wake blocked threads. */
            target->sig_pending |= (1ULL << sig);
            {
                extern void event_post(thread_t *target, uint32_t type, uint64_t data);
                thread_t *t = target->threads;
                while (t) {
                    if (t->state == THREAD_BLOCKED)
                        event_post(t, EQ_CHILD_EXITED, (uint64_t)sig);
                    t = t->proc_next;
                }
            }
            return 0;
        }
        /* Continue: SIGCONT — resume stopped threads */
        if (sig == SIGCONT) {
            /* Clear any pending stop signals */
            target->sig_pending &= ~((1ULL << SIGSTOP) | (1ULL << SIGTSTP) |
                                      (1ULL << SIGTTIN) | (1ULL << SIGTTOU));
            /* Resume stopped threads directly */
            {
                extern void sched_add(thread_t *t);
                thread_t *t = target->threads;
                int resumed = 0;
                while (t) {
                    if (t->state == THREAD_STOPPED) {
                        t->state = THREAD_RUNNABLE;
                        sched_add(t);
                        resumed = 1;
                    }
                    t = t->proc_next;
                }
                if (resumed) {
                    target->stop_signal = 0;
                    target->was_continued = 1;
                }
                /* Notify parent */
                if (resumed && target->parent_pid) {
                    process_t *parent = proc_find(target->parent_pid);
                    if (parent) {
                        thread_t *pt = parent->threads;
                        while (pt) {
                            event_post(pt, EQ_CHILD_CONTINUED, (uint64_t)target->pid);
                            pt = pt->proc_next;
                        }
                    }
                }
            }
            return 0;
        }

        /* Everything else (fatal).
         * Linux semantics: blocked signals stay pending, not delivered.
         * Check if ANY thread can receive this signal. */
        {
            int all_blocked = 1;
            thread_t *t = target->threads;
            while (t) {
                if (!((1ULL << sig) & t->sig_blocked)) { all_blocked = 0; break; }
                t = t->proc_next;
            }
            if (all_blocked) {
                /* Signal blocked on all threads — just set pending */
                target->sig_pending |= (1ULL << sig);
                return 0;
            }
        }

        /* Signal is unblocked — terminate immediately.
         * For proc_current(): direct exit (fast path).
         * For remote: mark zombie + kill threads + notify parent. */
        target->exit_signal = sig;
        if (target == proc_current()) {
            do_exit_group(128 + sig); /* doesn't return */
        }
        target->state = PROC_ZOMBIE;
        target->exit_code = 128 + sig;
        {
            thread_t *t = target->threads;
            while (t) {
                if (t->state == THREAD_BLOCKED || t->state == THREAD_RUNNING ||
                    t->state == THREAD_STOPPED)
                    t->state = THREAD_DEAD;
                t = t->proc_next;
            }
        }
        if (target->parent_pid) {
            process_t *parent = proc_find(target->parent_pid);
            if (parent) {
                extern void event_post(thread_t *target, uint32_t type, uint64_t data);
                uint64_t pflags;
                spin_lock_irq(&parent->lock, &pflags);
                thread_t *pt = parent->threads;
                while (pt) {
                    event_post(pt, 1 /* EQ_CHILD_EXITED */, 0);
                    pt = pt->proc_next;
                }
                spin_unlock_irq(&parent->lock, pflags);
            }
        }
        return 0;
    }

    /* User handler registered — set pending bit.
     * Delivery happens on return to userspace via check_pending_signals. */
    target->sig_pending |= (1ULL << sig);

    /* Wake blocked threads that have this signal unblocked.
     * event_post wakes via sched_wake (BLOCKED→RUNNABLE CAS). */
    {
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        thread_t *t = target->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED && !((1ULL << sig) & t->sig_blocked))
                event_post(t, 1 /* EQ_CHILD_EXITED */, (uint64_t)sig);
            t = t->proc_next;
        }
    }
    return 0;
}

/* Send signal to all processes in a process group. */
static long kill_pgrp(uint32_t pgid, int sig) {
    int found = 0;
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *p = proc_find((uint32_t)i);
        if (p && p->pgid == pgid && p->state == PROC_ALIVE) {
            kill_one(p, sig);
            found = 1;
        }
    }
    return found ? 0 : -ESRCH;
}

long do_kill(int pid, int sig) {
    if (sig < 0 || sig >= 64) return -EINVAL;
    if (sig == 0) return 0; /* check permission only */

    if (pid > 0) {
        /* Signal to specific process */
        process_t *target = proc_find((uint32_t)pid);
        if (!target) return -ESRCH;
        return kill_one(target, sig);
    } else if (pid == 0) {
        /* Signal to own process group */
        process_t *self = proc_current();
        if (!self) return -ESRCH;
        return kill_pgrp(self->pgid, sig);
    } else if (pid == -1) {
        /* Signal to all processes except init (pid 1) */
        int found = 0;
        for (int i = 1; i < PID_TABLE_MAX; i++) {
            process_t *p = proc_find((uint32_t)i);
            if (!p || p->state != PROC_ALIVE) continue;
            if (p->pid <= 1) continue; /* skip init */
            kill_one(p, sig);
            found = 1;
        }
        return found ? 0 : -ESRCH;
    } else {
        /* pid < -1: signal to process group abs(pid) */
        return kill_pgrp((uint32_t)(-pid), sig);
    }
}

/* ── SYS_TGKILL (234) — thread-directed signal ────── */

long do_tgkill(int tgid, int tid, int sig) {
    if (sig < 0 || sig >= 64) return -EINVAL;
    if (tgid <= 0 || tid <= 0) return -EINVAL;
    if (sig == 0) return 0; /* permission check only */

    /* Find target thread */
    thread_t *target = thread_find_by_tid(tid);
    if (!target || !target->proc) return -ESRCH;
    if ((int)target->proc->pid != tgid) return -EINVAL; /* tid not in tgid */

    process_t *p = target->proc;

    /* Check handler */
    void *handler = p->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    if (handler == SIG_DFL) {
        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, SIGIO */
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) return 0;
        /* Stop/Continue: delegate to kill_one (process-level) */
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU ||
            sig == SIGCONT)
            return kill_one(p, sig);

        /* Everything else (fatal): check if signal is blocked on target thread */
        if ((1ULL << sig) & target->sig_blocked) {
            p->sig_pending |= (1ULL << sig);
            return 0;
        }
        /* Signal is deliverable — terminate via kill_one (process-level) */
        return kill_one(p, sig);
    }

    /* User handler — set pending and wake target thread */
    p->sig_pending |= (1ULL << sig);
    if (!((1ULL << sig) & target->sig_blocked)) {
        extern void event_post(thread_t *tgt, uint32_t type, uint64_t data);
        if (target->state == THREAD_BLOCKED)
            event_post(target, 1 /* EQ_CHILD_EXITED */, (uint64_t)sig);
    }
    return 0;
}

/* ── SYS_TKILL (200) — send signal to thread (legacy, delegate to tgkill) ── */

long do_tkill(int tid, int sig) {
    thread_t *target = thread_find_by_tid(tid);
    if (!target || !target->proc) return -ESRCH;
    return do_tgkill((int)target->proc->pid, tid, sig);
}

/* ── SYS_RT_SIGPENDING (127) — return pending signal mask ── */

long do_rt_sigpending(uint64_t *set, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t || !t->proc) return -EFAULT;
    uint64_t pending = t->proc->sig_pending & ~t->sig_blocked;
    int r = copy_to_user(set, &pending, 8);
    if (r) return r;
    if (sigsetsize == 16) {
        uint64_t zero = 0;
        r = copy_to_user(set + 1, &zero, 8);
        if (r) return r;
    }
    return 0;
}

/* ── SYS_RT_SIGTIMEDWAIT (128) — wait for signal with timeout ── */

long do_rt_sigtimedwait(const uint64_t *uset, void *uinfo, const struct k_timespec *uts, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t || !t->proc) return -EFAULT;
    process_t *p = t->proc;

    uint64_t wait_mask;
    { int r = copy_from_user(&wait_mask, uset, 8); if (r) return r; }

    int timeout_ms = -1;
    if (uts) {
        struct k_timespec kts;
        int r = copy_from_user(&kts, uts, sizeof(kts));
        if (r) return r;
        timeout_ms = (int)(kts.tv_sec * 1000 + kts.tv_nsec / 1000000);
        if (timeout_ms < 0) timeout_ms = 0;
    }

    /* Check if any waited signal is already pending */
    uint64_t match = p->sig_pending & wait_mask;
    if (match) {
        int sig;
        for (sig = 1; sig < 64; sig++)
            if (match & (1ULL << sig)) break;
        p->sig_pending &= ~(1ULL << sig);
        /* Write siginfo (simplified: just si_signo at offset 0) */
        if (uinfo) {
            int ksi[32];
            kmemset(ksi, 0, sizeof(ksi));
            ksi[0] = sig; /* si_signo */
            copy_to_user(uinfo, ksi, 128);
        }
        return sig;
    }

    if (timeout_ms == 0) return -EAGAIN;

    /* Block, waiting for signal */
    {
        event_t ev;
        int r = event_wait(&t->eq, &ev, timeout_ms);
        if (r == -ETIMEDOUT) return -EAGAIN;
    }

    /* Re-check after wake */
    match = p->sig_pending & wait_mask;
    if (match) {
        int sig;
        for (sig = 1; sig < 64; sig++)
            if (match & (1ULL << sig)) break;
        p->sig_pending &= ~(1ULL << sig);
        if (uinfo) {
            int ksi[32];
            kmemset(ksi, 0, sizeof(ksi));
            ksi[0] = sig;
            copy_to_user(uinfo, ksi, 128);
        }
        return sig;
    }

    return -EAGAIN;
}

/* ── SYS_RT_SIGQUEUEINFO (129) — send signal with info ── */

long do_rt_sigqueueinfo(int pid, int sig, void *uinfo) {
    (void)uinfo; /* we don't use siginfo_t data beyond sig number */
    return do_kill(pid, sig);
}

/* ── SYS_RT_SIGSUSPEND (130) ────────────────────────── */

long do_rt_sigsuspend(const uint64_t *mask, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    process_t *p = t->proc;
    if (!p) return -EFAULT;
    if (!mask) return -EFAULT;
    uint64_t new_mask;
    { int r = copy_from_user(&new_mask, mask, 8); if (r) return r; }
    new_mask &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL/SIGSTOP never blocked */

    uint64_t old_blocked = t->sig_blocked;

    /* On syscall restart (in_sigsuspend already set), use saved mask as original.
     * sig_blocked is still the temp mask from the first invocation. */
    if (t->in_sigsuspend)
        old_blocked = t->sig_saved_mask;

    /* Install temporary mask */
    t->sig_blocked = new_mask;

    /* If a signal is already deliverable, restore and return */
    if (p->sig_pending & ~t->sig_blocked) {
        t->sig_blocked = old_blocked;
        t->in_sigsuspend = 0;
        return -EINTR;
    }

    /* Save old mask on thread so signal delivery path restores it */
    t->sig_saved_mask = old_blocked;
    t->in_sigsuspend = 1;

    /* Block via event_wait — do_kill will event_post us.
     * On wake, syscall restarts: sigsuspend re-checks pending signals. */
    {
        event_t ev;
        event_wait(&t->eq, &ev, -1);
    }
    return -EINTR; /* unreachable — syscall restarts */
}
