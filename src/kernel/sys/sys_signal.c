/* CosmoRT Syscall Layer — signal delivery, kill, mask operations */

#include "internal.h"
#include "core/event_queue.h"

void check_pending_signals(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;

    if (p->alarm_deadline_ms > 0 && timer_ms() >= p->alarm_deadline_ms) {
        p->alarm_deadline_ms = 0;
        void *handler = p->sig_actions[SIGALRM].sa_handler;
        if (handler == SIG_IGN) { }
        else if (handler == SIG_DFL) {
            p->exit_signal = SIGALRM;
            do_exit(128 + SIGALRM);
            return;
        } else {
            p->sig_pending |= SIG_BIT(SIGALRM);
        }
    }

    if (t->in_sigsuspend) {
        t->sig_blocked = t->sig_saved_mask;
        t->in_sigsuspend = 0;
    }

    uint64_t deliverable = (p->sig_pending | t->sig_thread_pending) & ~t->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < 64; sig++) {
        if (!(deliverable & SIG_BIT(sig))) continue;
        if (t->sig_thread_pending & SIG_BIT(sig))
            t->sig_thread_pending &= ~SIG_BIT(sig);
        else
            p->sig_pending &= ~SIG_BIT(sig);

        struct k_sigaction *sa = &p->sig_actions[sig];
        uint64_t handler = (uint64_t)sa->sa_handler;

        if (handler == 1) continue;

        if (handler == 0) {
            switch (sig) {
            case 23:
            case 28:
            case 29:
                p->sig_pending &= ~SIG_BIT(sig);
                continue;
            case 17:
                continue;
            case 19: case 20: case 21: case 22: {
                thread_t *th = p->threads;
                while (th) {
                    if (th->state == THREAD_RUNNING || th->state == THREAD_RUNNABLE)
                        th->state = THREAD_STOPPED;
                    th = th->proc_next;
                }
                p->stop_signal = sig;
                p->was_continued = 0;
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
                t->state = THREAD_STOPPED;
                return;
            }
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
            default:
                p->exit_signal = sig;
                if (percpu_self()->in_preempt) {
                    t->state = THREAD_DEAD;
                    p->state = PROC_ZOMBIE;
                    p->exit_code = 128 + sig;
                    {
                        thread_t *th = p->threads;
                        while (th) {
                            if (th != t && (th->state == THREAD_RUNNING ||
                                th->state == THREAD_RUNNABLE || th->state == THREAD_BLOCKED))
                                th->state = THREAD_DEAD;
                            th = th->proc_next;
                        }
                    }
                    if (p->parent_pid) {
                        process_t *parent = proc_find(p->parent_pid);
                        if (parent) {
                            thread_t *pt = parent->threads;
                            while (pt) {
                                event_post(pt, EQ_CHILD_EXITED, 0);
                                pt = pt->proc_next;
                            }
                        }
                    }
                    return;
                }
                do_exit(128 + sig);
            }
        }

        deliver_signal(t, sig);
        return;
    }
}

long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                              size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    if (oldset) {
        uint64_t tmp = t->sig_blocked;
        int r = copy_to_user(oldset, &tmp, 8);
        if (r) return r;
        if (sigsetsize == 16) {
            uint64_t zero = 0;
            r = copy_to_user(oldset + 1, &zero, 8);
            if (r) return r;
        }
    }

    if (set) {
        uint64_t k_set;
        int r = copy_from_user(&k_set, set, 8);
        if (r) return r;
        uint64_t mask = k_set;
        mask &= ~(SIG_BIT(9) | SIG_BIT(19));
        switch (how) {
        case 0: t->sig_blocked |= mask; break;
        case 1: t->sig_blocked &= ~mask; break;
        case 2: t->sig_blocked = mask; break;
        default: return -EINVAL;
        }
    }

    return 0;
}

static long kill_one(process_t *target, int sig) {
    if (target->state != PROC_ALIVE) return -ESRCH;

    void *handler = target->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    if (handler == SIG_DFL) {
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) return 0;
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
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
            if (target->parent_pid) {
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
        if (sig == SIGCONT) {
            target->sig_pending &= ~(SIG_BIT(SIGSTOP) | SIG_BIT(SIGTSTP) |
                                      SIG_BIT(SIGTTIN) | SIG_BIT(SIGTTOU));
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

        {
            int all_blocked = 1;
            thread_t *t = target->threads;
            while (t) {
                if (!(SIG_BIT(sig) & t->sig_blocked)) { all_blocked = 0; break; }
                t = t->proc_next;
            }
            if (all_blocked) {
                target->sig_pending |= SIG_BIT(sig);
                return 0;
            }
        }

        target->exit_signal = sig;
        target->sig_pending |= SIG_BIT(sig);
        if (target == proc_current()) {
            do_exit_group(128 + sig);
        }
        {
            extern void sched_wake(thread_t *t);
            thread_t *t = target->threads;
            while (t) {
                if (t->state == THREAD_BLOCKED)
                    sched_wake(t);
                t = t->proc_next;
            }
        }
        if (target->parent_pid) {
            process_t *parent = proc_find(target->parent_pid);
            if (parent) {
                uint64_t pflags;
                spin_lock_irq(&parent->lock, &pflags);
                thread_t *pt = parent->threads;
                while (pt) {
                    event_post(pt, EQ_CHILD_EXITED, 0);
                    pt = pt->proc_next;
                }
                spin_unlock_irq(&parent->lock, pflags);
            }
        }
        return 0;
    }

    target->sig_pending |= SIG_BIT(sig);

    {
        thread_t *t = target->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED && !(SIG_BIT(sig) & t->sig_blocked))
                event_post(t, EQ_CHILD_EXITED, (uint64_t)sig);
            t = t->proc_next;
        }
    }
    return 0;
}

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
    if (sig == 0) return 0;

    if (pid > 0) {
        process_t *target = proc_find((uint32_t)pid);
        if (!target) return -ESRCH;
        return kill_one(target, sig);
    } else if (pid == 0) {
        process_t *self = proc_current();
        if (!self) return -ESRCH;
        return kill_pgrp(self->pgid, sig);
    } else if (pid == -1) {
        int found = 0;
        for (int i = 1; i < PID_TABLE_MAX; i++) {
            process_t *p = proc_find((uint32_t)i);
            if (!p || p->state != PROC_ALIVE) continue;
            if (p->pid <= 1) continue;
            kill_one(p, sig);
            found = 1;
        }
        return found ? 0 : -ESRCH;
    } else {
        return kill_pgrp((uint32_t)(-pid), sig);
    }
}

long do_tgkill(int tgid, int tid, int sig) {
    if (sig < 0 || sig >= 64) return -EINVAL;
    if (tgid <= 0 || tid <= 0) return -EINVAL;
    if (sig == 0) return 0;

    thread_t *target = thread_find_by_tid(tid);
    if (!target || !target->proc) return -ESRCH;
    if ((int)target->proc->pid != tgid) return -EINVAL;

    process_t *p = target->proc;

    void *handler = p->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    if (handler == SIG_DFL) {
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) return 0;
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU ||
            sig == SIGCONT)
            return kill_one(p, sig);

        if (SIG_BIT(sig) & target->sig_blocked) {
            target->sig_thread_pending |= SIG_BIT(sig);
            return 0;
        }
        return kill_one(p, sig);
    }

    target->sig_thread_pending |= SIG_BIT(sig);
    if (!(SIG_BIT(sig) & target->sig_blocked)) {
        if (target->state == THREAD_BLOCKED)
            event_post(target, EQ_CHILD_EXITED, (uint64_t)sig);
        extern void sched_wake(thread_t *t);
        sched_wake(target);
    }
    return 0;
}

long do_tkill(int tid, int sig) {
    thread_t *target = thread_find_by_tid(tid);
    if (!target || !target->proc) return -ESRCH;
    return do_tgkill((int)target->proc->pid, tid, sig);
}

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

long do_rt_sigtimedwait(const uint64_t *uset, void *uinfo, const struct k_timespec *uts, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t || !t->proc) return -EFAULT;
    process_t *p = t->proc;

    uint64_t wait_mask;
    { int r = copy_from_user(&wait_mask, uset, 8); if (r) return r; }

    int timeout_ms = -1;
    if (uts) {
        if (t->nanosleep_deadline) {
            uint64_t now = timer_ms();
            if (now >= t->nanosleep_deadline) {
                t->nanosleep_deadline = 0;
                { event_t ev; while (event_wait(&t->eq, &ev, 0) == 0) { } }
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
                return -EAGAIN;
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
            t->nanosleep_deadline = timer_ms() + (uint64_t)timeout_ms;
        }
    }

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

    { event_t ev;
      while (event_wait(&t->eq, &ev, 0) == 0) { }
    }

    {
        event_t ev;
        int r = event_wait(&t->eq, &ev, timeout_ms);
        if (r == -ETIMEDOUT) { t->nanosleep_deadline = 0; return -EAGAIN; }
    }

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

long do_rt_sigqueueinfo(int pid, int sig, void *uinfo) {
    (void)uinfo;
    return do_kill(pid, sig);
}

long do_rt_sigsuspend(const uint64_t *mask, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    process_t *p = t->proc;
    if (!p) return -EFAULT;
    if (!mask) return -EFAULT;
    uint64_t new_mask;
    { int r = copy_from_user(&new_mask, mask, 8); if (r) return r; }
    new_mask &= ~(SIG_BIT(9) | SIG_BIT(19));

    uint64_t old_blocked = t->sig_blocked;

    if (t->in_sigsuspend)
        old_blocked = t->sig_saved_mask;

    t->sig_blocked = new_mask;

    if (p->sig_pending & ~t->sig_blocked) {
        t->sig_blocked = old_blocked;
        t->in_sigsuspend = 0;
        return -EINTR;
    }

    t->sig_saved_mask = old_blocked;
    t->in_sigsuspend = 1;

    {
        event_t ev;
        int _wr = event_wait(&t->eq, &ev, -1);
        if (_wr == -4) return -EINTR;
    }
    return -EINTR;
}
