/* CosmoRT Syscall Layer — signal delivery, kill, mask operations */

#include "sys/internal.h"
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
            p->sig_pending |= SIG_BIT(SIGALRM);
        }
    }

    /* If returning from rt_sigsuspend, restore the original mask before
     * delivering the signal so the ucontext captures the pre-sigsuspend mask. */
    if (t->in_sigsuspend) {
        t->sig_blocked = t->sig_saved_mask;
        t->in_sigsuspend = 0;
    }

    uint64_t deliverable = (p->sig_pending | t->sig_thread_pending) & ~t->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < 64; sig++) {
        if (!(deliverable & SIG_BIT(sig))) continue;
        /* Clear from whichever pending set has it (thread-level takes priority) */
        if (t->sig_thread_pending & SIG_BIT(sig))
            t->sig_thread_pending &= ~SIG_BIT(sig);
        else
            p->sig_pending &= ~SIG_BIT(sig);

        struct k_sigaction *sa = &p->sig_actions[sig];
        uint64_t handler = (uint64_t)sa->sa_handler;

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
                /* Current thread stopped.
                 * Set state and notify parent. Callers handle the actual yield:
                 * - sched_preempt: longjmp to scheduler (state != RUNNING → not re-queued)
                 * - check_signals_syscall_path: save state and thread_return_to_kernel
                 * Both callers sync frame ↔ thread_t, so register state is preserved. */
                t->state = THREAD_STOPPED;
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
                /* Preempt context: can't call do_exit (would corrupt IRQ frame).
                 * Kill all threads, set zombie state, notify parent.
                 * sched_preempt sees state!=RUNNING → longjmp without re-queue. */
                if (percpu_self()->in_preempt) {
                    t->state = THREAD_DEAD;
                    p->state = PROC_ZOMBIE;
                    p->exit_code = 128 + sig;
                    /* Kill sibling threads */
                    {
                        thread_t *th = p->threads;
                        while (th) {
                            if (th != t && (th->state == THREAD_RUNNING ||
                                th->state == THREAD_RUNNABLE || th->state == THREAD_BLOCKED))
                                th->state = THREAD_DEAD;
                            th = th->proc_next;
                        }
                    }
                    /* Notify parent */
                    if (p->parent_pid) {
                        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
                        process_t *parent = proc_find(p->parent_pid);
                        if (parent) {
                            thread_t *pt = parent->threads;
                            while (pt) {
                                event_post(pt, 1 /* EQ_CHILD_EXITED */, 0);
                                pt = pt->proc_next;
                            }
                        }
                    }
                    return;
                }
                do_exit(128 + sig); /* syscall context — doesn't return */
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
        mask &= ~(SIG_BIT(9) | SIG_BIT(19)); /* SIGKILL, SIGSTOP cannot be blocked */
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
long kill_one(process_t *target, int sig) {
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
            /* Stop all threads immediately — don't defer to check_pending_signals.
             * Threads on other cores will be preempted and see THREAD_STOPPED. */
            {
                extern void event_post(thread_t *t, uint32_t type, uint64_t data);
                thread_t *t = target->threads;
                while (t) {
                    if (t->state == THREAD_RUNNING || t->state == THREAD_RUNNABLE)
                        t->state = THREAD_STOPPED;
                    else if (t->state == THREAD_BLOCKED)
                        t->state = THREAD_STOPPED;
                    t = t->proc_next;
                }
            }
            target->stop_signal = sig;
            target->was_continued = 0;
            /* Notify parent */
            if (target->parent_pid) {
                extern void event_post(thread_t *t, uint32_t type, uint64_t data);
                process_t *parent = proc_find(target->parent_pid);
                if (parent) {
                    thread_t *pt = parent->threads;
                    while (pt) {
                        event_post(pt, EQ_CHILD_STOPPED, ((uint64_t)sig << 32) | target->pid);
                        pt = pt->proc_next;
                    }
                }
            }
            return 0;
        }
        /* Continue: SIGCONT — resume stopped threads */
        if (sig == SIGCONT) {
            /* Clear any pending stop signals */
            target->sig_pending &= ~(SIG_BIT(SIGSTOP) | SIG_BIT(SIGTSTP) |
                                      SIG_BIT(SIGTTIN) | SIG_BIT(SIGTTOU));
            /* Resume stopped threads. Set was_continued BEFORE sched_add so a
             * concurrent wait4(WCONTINUED) on a fast-scheduled child sees the
             * flag even if the child reaches PROC_ZOMBIE between sched_add
             * and the flag-update. */
            {
                extern void sched_add(thread_t *t);
                thread_t *t = target->threads;
                int resumed = 0;
                for (t = target->threads; t; t = t->proc_next) {
                    if (t->state == THREAD_STOPPED) { resumed = 1; break; }
                }
                if (resumed) {
                    target->stop_signal = 0;
                    target->was_continued = 1;
                }
                for (t = target->threads; t; t = t->proc_next) {
                    if (t->state == THREAD_STOPPED) {
                        t->state = THREAD_RUNNABLE;
                        sched_add(t);
                    }
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
                if (!(SIG_BIT(sig) & t->sig_blocked)) { all_blocked = 0; break; }
                t = t->proc_next;
            }
            if (all_blocked) {
                /* Signal blocked on all threads — just set pending */
                target->sig_pending |= SIG_BIT(sig);
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
            extern void sched_wake(thread_t *t);
            thread_t *t = target->threads;
            while (t) {
                int old = t->state;
                if (old == THREAD_BLOCKED) {
                    sched_wake(t);
                    t->state = THREAD_DEAD;
                } else if (old == THREAD_RUNNING || old == THREAD_STOPPED ||
                           old == THREAD_RUNNABLE) {
                    t->state = THREAD_DEAD;
                }
                t = t->proc_next;
            }
        }
        if (target->parent_pid) {
            process_t *parent = proc_find(target->parent_pid);
            if (parent) {
                int nsig = target->notify_signal ? target->notify_signal : SIGCHLD;
                __sync_fetch_and_or(&parent->sig_pending, SIG_BIT(nsig));
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
    target->sig_pending |= SIG_BIT(sig);

    /* Wake blocked threads that have this signal unblocked.
     * event_post wakes via sched_wake (BLOCKED→RUNNABLE CAS). */
    {
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        thread_t *t = target->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED && !(SIG_BIT(sig) & t->sig_blocked))
                event_post(t, 1 /* EQ_CHILD_EXITED */, (uint64_t)sig);
            t = t->proc_next;
        }
    }
    return 0;
}

/* Send signal to all processes in a process group. */
static long kill_pgrp(uint32_t pgid, int sig) {
    int found = 0;
    int cap = pid_table_capacity();
    for (int i = 1; i < cap; i++) {
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
        int cap = pid_table_capacity();
        for (int i = 1; i < cap; i++) {
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
        if (SIG_BIT(sig) & target->sig_blocked) {
            target->sig_thread_pending |= SIG_BIT(sig);
            return 0;
        }
        /* Signal is deliverable — terminate via kill_one (process-level) */
        return kill_one(p, sig);
    }

    /* User handler — set per-thread pending and wake target thread.
     * tgkill targets a specific thread, so use thread-level pending
     * (not process-level) to ensure the correct thread handles it. */
    target->sig_thread_pending |= SIG_BIT(sig);
    if (!(SIG_BIT(sig) & target->sig_blocked)) {
        extern void event_post(thread_t *tgt, uint32_t type, uint64_t data);
        if (target->state == THREAD_BLOCKED)
            event_post(target, 1 /* EQ_CHILD_EXITED */, (uint64_t)sig);
        extern void sched_wake(thread_t *t);
        sched_wake(target);
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

    /* Compute deadline once, use remaining time on syscall restart */
    int timeout_ms = -1;
    if (uts) {
        if (t->nanosleep_deadline) {
            /* Restarted after blocking — use remaining time */
            uint64_t now = timer_ms();
            if (now >= t->nanosleep_deadline) {
                t->nanosleep_deadline = 0;
                /* Drain stale events from previous block */
                { event_t ev; while (event_wait(&t->eq, &ev, 0) == 0) { } }
                /* Check one more time before returning */
                uint64_t match2 = p->sig_pending & wait_mask;
                if (match2) {
                    int sig;
                    for (sig = 1; sig < 64; sig++)
                        if (match2 & SIG_BIT(sig)) break;
                    p->sig_pending &= ~SIG_BIT(sig);
                    if (uinfo) {
                        int ksi[32]; kmemset(ksi, 0, sizeof(ksi));
                        ksi[0] = sig;
                        copy_to_user(uinfo, ksi, 128);
                    }
                    return sig;
                }
                return -EAGAIN; /* timeout expired */
            }
            timeout_ms = (int)(t->nanosleep_deadline - now);
            if (timeout_ms <= 0) timeout_ms = 1;
        } else {
            struct k_timespec kts;
            int r = copy_from_user(&kts, uts, sizeof(kts));
            if (r) return r;
            timeout_ms = (int)(kts.tv_sec * 1000 + kts.tv_nsec / 1000000);
            if (timeout_ms < 0) timeout_ms = 0;
            if (timeout_ms == 0) {
                /* Check pending and return immediately */
                uint64_t match = p->sig_pending & wait_mask;
                if (match) {
                    int sig;
                    for (sig = 1; sig < 64; sig++)
                        if (match & SIG_BIT(sig)) break;
                    p->sig_pending &= ~SIG_BIT(sig);
                    if (uinfo) {
                        int ksi[32]; kmemset(ksi, 0, sizeof(ksi));
                        ksi[0] = sig;
                        copy_to_user(uinfo, ksi, 128);
                    }
                    return sig;
                }
                return -EAGAIN;
            }
            /* Set deadline for future restarts */
            t->nanosleep_deadline = timer_ms() + (uint64_t)timeout_ms;
        }
    }

    /* Check if any waited signal is already pending */
    uint64_t match = p->sig_pending & wait_mask;
    if (match) {
        int sig;
        for (sig = 1; sig < 64; sig++)
            if (match & SIG_BIT(sig)) break;
        p->sig_pending &= ~SIG_BIT(sig);
        if (uinfo) {
            int ksi[32];
            kmemset(ksi, 0, sizeof(ksi));
            ksi[0] = sig;
            copy_to_user(uinfo, ksi, 128);
        }
        t->nanosleep_deadline = 0;
        return sig;
    }

    /* Drain stale events before blocking */
    { event_t ev;
      while (event_wait(&t->eq, &ev, 0) == 0) { /* drain */ }
    }

    /* Block, waiting for signal */
    {
        event_t ev;
        int r = event_wait(&t->eq, &ev, timeout_ms);
        if (r == -ETIMEDOUT) { t->nanosleep_deadline = 0; return -EAGAIN; }
    }

    /* Re-check after wake (dead code — event_wait uses syscall restart,
     * so the function re-enters from the top on wake. Kept for clarity.) */
    match = p->sig_pending & wait_mask;
    if (match) {
        int sig;
        for (sig = 1; sig < 64; sig++)
            if (match & SIG_BIT(sig)) break;
        p->sig_pending &= ~SIG_BIT(sig);
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
    new_mask &= ~(SIG_BIT(9) | SIG_BIT(19)); /* SIGKILL/SIGSTOP never blocked */

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
        int _wr = event_wait(&t->eq, &ev, -1);
        if (_wr == -4) return -EINTR;
    }
    return -EINTR; /* unreachable — syscall restarts */
}
