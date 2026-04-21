/* CosmoRT Process — wait4, process cleanup */

#include "proc/proc_internal.h"

/* ── Process cleanup (2.3) ───────────────────────── */

void proc_cleanup(process_t *p) {
    if (!p) return;

    /* Release all advisory file locks held by this process */
    extern void flock_release_pid(uint32_t pid);
    flock_release_pid(p->pid);

    /* Close all FDs — decrement refcount, free when last ref.
     * exit_kill_process already does this, so entries may be FD_NONE. */
    for (int i = 0; i < p->fds.max_slots; i++) {
        int type = p->fds.entries[i].type;
        if (type == FD_FILE) {
            vfs_file_free_obj(p->fds.entries[i].obj);
        } else if (type != FD_NONE && type != FD_SERIAL) {
            fd_cleanup_entry(type, p->fds.entries[i].obj,
                             p->fds.entries[i].flags);
        }
        p->fds.entries[i].type = FD_NONE;
        p->fds.entries[i].obj = 0;
    }
    fd_table_free(&p->fds);

    /* Free threads.
     * exit_kill_process sets sibling threads to DEAD with proc=NULL. Those
     * threads may still be in the run queue — the scheduler drains and frees
     * them lazily (see sched_loop). Only free threads still owned by this
     * process (proc != NULL), which means exit_kill_process didn't orphan them.
     * The main thread (which executed exit_group) is always safe: it did
     * longjmp back to sched_loop and was not re-enqueued. */
    thread_t *t = p->threads;
    while (t) {
        thread_t *next = t->proc_next;
        if (t->proc == p) {
            /* Still owned by this process — safe to free */
            t->state = THREAD_DEAD;
            thread_free(t);
        }
        /* else: orphaned by exit_kill_process (proc=NULL), scheduler frees it */
        t = next;
    }
    p->threads = 0;
    p->main_thread = 0;
    p->thread_count = 0;

    /* Shared VMA pages belong to the allocator — clear PTEs so
     * free_address_space doesn't double-free them. */
    unmap_shared_vmas(p->vma_root, p->pml4);

    /* Free address space (pages + page tables) */
    free_address_space(p->pml4);
    p->pml4 = 0;

    /* Free VMAs */
    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    /* Clear lookup table entry */
    if ((int)p->pid < pid_table_capacity())
        pid_table[p->pid] = 0;

    /* Free process struct */
    slab_free(&proc_slab, p);
}

/* ── wait4 (2.2) ─────────────────────────────────── */

#define SA_NOCLDWAIT_VAL 0x00000002

long do_wait4(int pid, int *wstatus, int options, void *rusage) {
    (void)rusage;

    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *parent = cur->proc;

    /* SA_NOCLDWAIT + SIG_DFL for SIGCHLD: children are auto-reaped,
     * wait4 returns -ECHILD immediately.
     * SA_NOCLDWAIT + handler: children are still reaped via wait4, but
     * become zombies only briefly. We handle the SIG_DFL case here. */
    if ((parent->sig_actions[17 /* SIGCHLD */].sa_flags & SA_NOCLDWAIT_VAL) &&
        parent->sig_actions[17].sa_handler == (void *)0 /* SIG_DFL */) {
        /* Auto-reap all zombie children matching pid filter */
        int cap = pid_table_capacity();
        for (int i = 1; i < cap; i++) {
            process_t *child = pid_table[i];
            if (!child || child->parent_pid != parent->pid) continue;
            if (child->state == PROC_ZOMBIE) proc_cleanup(child);
        }
        return -ECHILD;
    }

    if (wstatus && !((uint64_t)wstatus < 0x800000000000ULL &&
                     (uint64_t)wstatus + sizeof(int) <= 0x800000000000ULL &&
                     (uint64_t)wstatus + sizeof(int) >= (uint64_t)wstatus))
        return -EFAULT;

    int wnohang    = options & 1;  /* WNOHANG = 1 */
    int wuntraced  = options & 2;  /* WUNTRACED = 2 */
    int wcontinued = options & 8;  /* WCONTINUED = 8 */

    /* Scan for matching child */
    for (;;) {
        int found_child = 0;
        int cap = pid_table_capacity();

        for (int i = 1; i < cap; i++) {
            process_t *child = pid_table[i];
            if (!child || child->state == PROC_FREE) continue;
            if (child->pid != (uint32_t)i) continue;
            if (child->parent_pid != parent->pid) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;
            if (pid == 0 || pid == -1) { /* wait for any child */ }

            found_child = 1;

            /* WUNTRACED: report stopped children */
            if (wuntraced && child->stop_signal) {
                int child_pid = (int)child->pid;
                int stop_sig = child->stop_signal;
                child->stop_signal = 0; /* consume — one report per stop */
                if (wstatus) {
                    int kstatus = (stop_sig << 8) | 0x7F; /* WIFSTOPPED */
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }
                return child_pid;
            }

            /* WCONTINUED: report continued children */
            if (wcontinued && child->was_continued) {
                int child_pid = (int)child->pid;
                child->was_continued = 0; /* consume */
                if (wstatus) {
                    int kstatus = 0xFFFF; /* WIFCONTINUED */
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }
                return child_pid;
            }

            if (child->state == PROC_ZOMBIE) {
                int child_pid = (int)child->pid;
                int exit_status = child->exit_code;

                if (wstatus) {
                    int kstatus;
                    if (child->exit_signal)
                        kstatus = child->exit_signal & 0x7F; /* killed by signal */
                    else
                        kstatus = (exit_status & 0xFF) << 8;  /* normal exit */
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }

                proc_cleanup(child);
                return child_pid;
            }
        }

        if (!found_child) return -ECHILD;
        if (wnohang) return 0;

        /* Block until child state change.
         * Temporarily suppress SIGCHLD during event_wait to prevent spurious
         * EINTR (we handle child state changes ourselves by re-scanning).
         * If a user handler is registered, re-post SIGCHLD after wait so
         * check_signals_syscall_path delivers it on syscall return. */
        {
            (void)parent; /* sig_actions checked later by check_signals_syscall_path */

            /* Suppress SIGCHLD for event_wait (prevent -EINTR loop) */
            cur->sig_blocked |= SIG_BIT(SIGCHLD);

            event_t ev;
            int _wr = event_wait(&cur->eq, &ev, -1);

            /* Restore SIGCHLD unblocked */
            cur->sig_blocked &= ~SIG_BIT(SIGCHLD);

            if (_wr == -4) {
                /* Signal interrupted wait. If non-SIGCHLD signals remain,
                 * return -EINTR. Otherwise re-scan children. */
                uint64_t remaining = (parent->sig_pending | cur->sig_thread_pending)
                                   & ~cur->sig_blocked & ~SIG_BIT(SIGCHLD);
                if (remaining)
                    return -EINTR;
            }
        }
    }
}
