/* CosmoRT Syscall Layer — signal delivery, kill, mask operations */

#include "sys/internal.h"
#include "core/waitqueue.h"
#include "core/hrtimer.h"

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
            __sync_fetch_and_or(&p->sig_pending, SIG_BIT(SIGALRM));
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
        /* Clear from whichever pending set has it (thread-level takes priority).
         * Atomic RMW: kill_one and check_alarm_timers can fire from IRQ ctx and
         * race with this clear; non-atomic |= would lose a freshly-set bit. */
        if (t->sig_thread_pending & SIG_BIT(sig))
            __sync_fetch_and_and(&t->sig_thread_pending, ~SIG_BIT(sig));
        else
            __sync_fetch_and_and(&p->sig_pending, ~SIG_BIT(sig));

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
                    if (parent) wake_up(&parent->children_wq);
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
                    if (parent) wake_up(&parent->children_wq);
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
                        process_t *parent = proc_find(p->parent_pid);
                        if (parent) wake_up(&parent->children_wq);
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
        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, SIGIO.
         * Linux-Kompatibilitaet: wenn das Signal auf allen Threads
         * blockiert ist (z.B. sigprocmask() + sigtimedwait()), traegt
         * der Kernel es trotzdem als pending ein — sigtimedwait
         * konsumiert es dann, bevor die Default-Ignore-Aktion greift. */
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) {
            int any_blocked = 0;
            thread_t *t = target->threads;
            while (t) {
                if (SIG_BIT(sig) & t->sig_blocked) { any_blocked = 1; break; }
                t = t->proc_next;
            }
            if (any_blocked) {
                __sync_fetch_and_or(&target->sig_pending, SIG_BIT(sig));
                /* sigtimedwait blockt auf einer eigenen lokalen wq und
                 * weckt sich selbst durch try_to_wake_up + signal_pending-
                 * Recheck. signal_wake_up macht den state-CAS direkt; die
                 * sigtimedwait-Loop entdeckt match nach schedule()-Return. */
                thread_t *w = target->threads;
                while (w) {
                    if (w->state == THREAD_BLOCKED)
                        signal_wake_up(w);
                    w = w->proc_next;
                }
            }
            return 0;
        }
        /* Stop: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
            /* Stop all threads immediately — don't defer to check_pending_signals.
             * Threads on other cores will be preempted and see THREAD_STOPPED. */
            {
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
                process_t *parent = proc_find(target->parent_pid);
                if (parent) wake_up(&parent->children_wq);
            }
            return 0;
        }
        /* Continue: SIGCONT — resume stopped threads */
        if (sig == SIGCONT) {
            /* Clear any pending stop signals (atomic — IRQ-safe RMW) */
            __sync_fetch_and_and(&target->sig_pending,
                ~(SIG_BIT(SIGSTOP) | SIG_BIT(SIGTSTP) |
                  SIG_BIT(SIGTTIN) | SIG_BIT(SIGTTOU)));
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
                    if (parent) wake_up(&parent->children_wq);
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
                /* Blocked auf allen Threads → sigtimedwait-Pfad. Sleeper
                 * parkt auf lokaler wq mit signal_pending-Recheck; ein
                 * direkter state-CAS via signal_wake_up genuegt, die Loop
                 * findet das Signal nach schedule()-Return. */
                __sync_fetch_and_or(&target->sig_pending, SIG_BIT(sig));
                thread_t *wt = target->threads;
                while (wt) {
                    if (wt->state == THREAD_BLOCKED)
                        signal_wake_up(wt);
                    wt = wt->proc_next;
                }
                return 0;
            }
        }

        /* Signal is unblocked — terminate.
         * Self-target (kill(getpid(), SIGKILL) etc.): direct do_exit_group fast path.
         * Remote target: Linux-Modell — sig_pending + signal_wake_up auf jeden
         * Thread. Empfaenger sieht das Signal beim naechsten check_pending_signals
         * (Syscall-Exit, Preempt-Tick, oder Resched-IPI-Pfad) und ruft
         * do_exit_group SELBST auf — durchlaeuft den vollen exit_kill_process-
         * Pfad mit allen Cleanups (FD-close, vfork-wake, robust_list, flock,
         * Reparenting, Address-Space-Teardown).
         *
         * Direktes ZOMBIE-Setzen aus remote-Kontext wuerde all das ueberspringen
         * und blockierte Pipe-Reader auf der anderen Seite haengen lassen.
         *
         * Cross-CPU-Latency: signal_wake_up sendet smp_resched_others NUR wenn
         * mindestens ein Thread BLOCKED war (try_to_wake_up=1). Bei Prozessen
         * mit ausschliesslich RUNNING-Workers (ebizzy: pthread-busyloop ohne
         * Syscalls, Master in pthread_join-futex) muessen die Worker den
         * naechsten 1ms-Timer-Tick auf ihrer CPU abwarten — bei volllast-
         * akkumulierter Test-Sequenz koppelt das mit anderen Latenzen zu
         * mehrsekuendigen Hangs. Daher: smp_resched_others UNBEDINGT feuern,
         * damit der Resched-IPI-Pfad (irq.c:599) sched_preempt → check_pending_
         * signals auf jeder anderen CPU triggert. */
        target->exit_signal = sig;
        if (target == proc_current()) {
            do_exit_group(128 + sig); /* doesn't return */
        }
        __sync_fetch_and_or(&target->sig_pending, SIG_BIT(sig));
        {
            thread_t *t = target->threads;
            while (t) {
                /* signal_wake_up: BLOCKED → RUNNABLE + sched_add. RUNNING/
                 * RUNNABLE/STOPPED bleiben unberuehrt — sie sehen sig_pending
                 * via Preempt-Tick / Syscall-Exit / Resched-IPI (siehe unten). */
                signal_wake_up(t);
                t = t->proc_next;
            }
        }
        /* Force IPI broadcast: garantiert dass JEDE andere CPU promptly aus
         * HLT/Userspace zurueckkehrt und sched_preempt ausfuehrt — der Pfad
         * triggert check_pending_signals auf user-mode-Threads. Ohne diesen
         * unbedingten IPI sieht ein RUNNING-Thread auf einer anderen CPU
         * sig_pending erst beim naechsten 1ms-Timer-Tick. */
        {
            extern void smp_resched_others(void);
            smp_resched_others();
        }
        return 0;
    }

    /* User handler registered — set pending bit.
     * Delivery happens on return to userspace via check_pending_signals.
     * Atomic RMW: kill_one races with check_pending_signals' &= ~bit
     * clear and check_alarm_timers' SIGALRM |= bit set; non-atomic |=
     * loses one of those bits when both fire close together. */
    __sync_fetch_and_or(&target->sig_pending, SIG_BIT(sig));

    /* Wake blocked threads that have this signal unblocked. Eine Primitive:
     * signal_wake_up macht try_to_wake_up(t, TASK_INTERRUPTIBLE | TASK_KILLABLE),
     * also state-CAS BLOCKED→RUNNABLE. Der Sleep-Loop des Empfaengers
     * (nanosleep / futex / socket-recv / sigtimedwait) checkt nach Return aus
     * schedule() seinen signal_pending-Filter und liefert EINTR oder konsumiert. */
    {
        thread_t *t = target->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED && !(SIG_BIT(sig) & t->sig_blocked))
                signal_wake_up(t);
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
        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, SIGIO — pending-Bit setzen
         * wenn blockiert (sigtimedwait-Pfad). Siehe kill_one-Kommentar. */
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) {
            if (SIG_BIT(sig) & target->sig_blocked) {
                __sync_fetch_and_or(&target->sig_thread_pending, SIG_BIT(sig));
                if (target->state == THREAD_BLOCKED)
                    signal_wake_up(target);
            }
            return 0;
        }
        /* Stop/Continue: delegate to kill_one (process-level) */
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU ||
            sig == SIGCONT)
            return kill_one(p, sig);

        /* Everything else (fatal): check if signal is blocked on target thread */
        if (SIG_BIT(sig) & target->sig_blocked) {
            __sync_fetch_and_or(&target->sig_thread_pending, SIG_BIT(sig));
            return 0;
        }
        /* Signal is deliverable — terminate via kill_one (process-level) */
        return kill_one(p, sig);
    }

    /* User handler — set per-thread pending and wake target thread.
     * tgkill targets a specific thread, so use thread-level pending
     * (not process-level) to ensure the correct thread handles it. */
    __sync_fetch_and_or(&target->sig_thread_pending, SIG_BIT(sig));
    if (!(SIG_BIT(sig) & target->sig_blocked))
        signal_wake_up(target);
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

/* hrtimer-Callback: bei Timeout wird die Sleeper-wq geweckt. Der Sleeper-
 * Loop checkt timer_ms() >= deadline_ms und returnt -EAGAIN. */
static void sigwait_timer_fn(hrtimer_t *timer) {
    wait_queue_head_t *wq = (wait_queue_head_t *)timer->data;
    if (wq) wake_up(wq);
}

/* Pull lowest-numbered matching signal from process- oder thread-pending,
 * fill siginfo_t (incl. dnotify SI_POLL), re-pend wenn weitere dnotify-
 * Eintraege fuer denselben sig in der Queue stecken (RT-Queue-Semantik).
 * Return: sig number (>0) wenn match, 0 wenn nichts pending. */
static int sigwait_consume(thread_t *t, process_t *p, uint64_t wait_mask,
                           void *uinfo) {
    uint64_t match = (p->sig_pending | t->sig_thread_pending) & wait_mask;
    if (!match) return 0;

    int sig;
    for (sig = 1; sig < 64; sig++)
        if (match & SIG_BIT(sig)) break;

    if (t->sig_thread_pending & SIG_BIT(sig))
        __sync_fetch_and_and(&t->sig_thread_pending, ~SIG_BIT(sig));
    else
        __sync_fetch_and_and(&p->sig_pending, ~SIG_BIT(sig));

    extern int dnotify_queue_pop_fd(process_t *p, int sig);
    extern int dnotify_queue_peek_fd(process_t *p, int sig);
    int dn_fd = dnotify_queue_pop_fd(p, sig);
    if (uinfo) {
        int ksi[32];
        kmemset(ksi, 0, sizeof(ksi));
        ksi[0] = sig;
        if (dn_fd >= 0) {
            ksi[2] = -6; /* SI_POLL */
            long band = 0;
            kmemcpy((char *)ksi + 16, &band, 8);
            kmemcpy((char *)ksi + 24, &dn_fd, 4);
        }
        copy_to_user(uinfo, ksi, 128);
    }
    if (dn_fd >= 0 && dnotify_queue_peek_fd(p, sig) >= 0)
        __sync_fetch_and_or(&p->sig_pending, SIG_BIT(sig));
    return sig;
}

long do_rt_sigtimedwait(const uint64_t *uset, void *uinfo, const struct k_timespec *uts, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t || !t->proc) return -EFAULT;
    process_t *p = t->proc;

    uint64_t wait_mask;
    { int r = copy_from_user(&wait_mask, uset, 8); if (r) return r; }

    /* Deadline ist absolute monotonic ms; 0 == infinity. nanosleep_deadline
     * persistiert ueber syscall-restart. */
    uint64_t deadline_ms = 0;
    int has_timeout = 0;
    if (uts) {
        has_timeout = 1;
        if (t->nanosleep_deadline) {
            deadline_ms = t->nanosleep_deadline;
        } else {
            struct k_timespec kts;
            int r = copy_from_user(&kts, uts, sizeof(kts));
            if (r) return r;
            long ms = (long)(kts.tv_sec * 1000 + kts.tv_nsec / 1000000);
            if (ms < 0) ms = 0;
            if (ms == 0) {
                /* Polling: check pending and return immediately */
                int sig = sigwait_consume(t, p, wait_mask, uinfo);
                return sig ? (long)sig : -EAGAIN;
            }
            deadline_ms = timer_ms() + (uint64_t)ms;
            t->nanosleep_deadline = deadline_ms;
        }
    }

    /* Fast path: signal already pending */
    {
        int sig = sigwait_consume(t, p, wait_mask, uinfo);
        if (sig) { t->nanosleep_deadline = 0; return sig; }
    }

    /* Block-Loop. Lokale wq, kein Routing — signal_wake_up macht state-CAS
     * direkt (kill_one/tgkill rufen es). hrtimer treibt die Timeout-Kante:
     * wq_timeout_fn weckt local_wq, schedule() returnt. Out-of-set deliverable
     * Signale terminieren mit -EINTR (POSIX: sigtimedwait wird von
     * Signalen ausserhalb der waitset unterbrochen). */
    wait_queue_head_t local_wq = WAIT_QUEUE_HEAD_INIT;
    init_waitqueue_head(&local_wq);
    DEFINE_WAIT(wait);
    hrtimer_t timer;
    int has_timer = 0;
    if (has_timeout) {
        uint64_t deadline_ns = ms_to_ns(deadline_ms);
        hrtimer_init(&timer, sigwait_timer_fn, &local_wq);
        hrtimer_start(&timer, deadline_ns);
        has_timer = 1;
    }

    long rc;
    for (;;) {
        prepare_to_wait(&local_wq, &wait, /*THREAD_BLOCKED*/ 3);

        int sig = sigwait_consume(t, p, wait_mask, uinfo);
        if (sig) { rc = sig; break; }

        if (has_timeout && timer_ms() >= deadline_ms) {
            rc = -EAGAIN;
            break;
        }

        uint64_t deliverable = (p->sig_pending | t->sig_thread_pending)
                             & ~t->sig_blocked;
        if (deliverable & ~wait_mask) { rc = -EINTR; break; }

        schedule();
    }
    finish_wait(&local_wq, &wait);
    if (has_timer) hrtimer_cancel(&timer);
    t->nanosleep_deadline = 0;
    return rc;
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

    /* Block bis ein deliverable Signal eintrifft. signal_wake_up macht den
     * state-CAS direkt; nach schedule()-Return prueft die Loop ob jetzt
     * etwas im sig_pending ist, das durch die temp-Maske durchkommt. */
    wait_queue_head_t local_wq = WAIT_QUEUE_HEAD_INIT;
    init_waitqueue_head(&local_wq);
    DEFINE_WAIT(wait);
    for (;;) {
        prepare_to_wait(&local_wq, &wait, /*THREAD_BLOCKED*/ 3);
        if ((p->sig_pending | t->sig_thread_pending) & ~t->sig_blocked) break;
        schedule();
    }
    finish_wait(&local_wq, &wait);
    return -EINTR;
}
