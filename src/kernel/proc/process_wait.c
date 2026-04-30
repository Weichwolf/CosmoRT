/* CosmoRT Process — wait4, process cleanup */

#include "proc/proc_internal.h"
#include "core/time_ns.h"
#include "core/waitqueue.h"
#include "net/net_ns.h"
#include "linux/signal.h"

/* ── Process cleanup (2.3) ───────────────────────── */

void proc_cleanup(process_t *p) {
    if (!p) return;

    /* Release i_writecount deny-write held by the process's text image. */
    if (p->exe_ino) {
        i_writecount_allow_write(p->exe_backend, p->exe_ino);
        p->exe_ino = 0;
    }

    /* Release all advisory file locks held by this process */
    extern void flock_release_pid(uint32_t pid);
    flock_release_pid(p->pid);

    /* Release dnotify-Watches des Prozesses */
    extern void dnotify_proc_exit(struct process *p);
    dnotify_proc_exit(p);

    /* Drop our reference on the fd_table. If shared (clone(CLONE_FILES))
     * the other holders keep their open fds; only the last drop closes every
     * entry and frees the backing pages. exit_kill_process already cleared
     * our half of the entries when this process is the sole holder. */
    if (p->fds) { fd_table_put(p->fds); p->fds = 0; }

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

    if (!p->mm_shared) {
        /* Shared VMA pages belong to the allocator — clear PTEs so
         * free_address_space doesn't double-free them. */
        unmap_shared_vmas(p->vma_root, p->pml4);

        /* Free address space (pages + page tables) */
        free_address_space(p->pml4);

        /* Free VMAs */
        vma_free_tree(p->vma_root);
    }
    p->pml4 = 0;
    p->vma_root = 0;

    /* Drop time_namespace references (init_time_ns is pinned). */
    if (p->time_ns)              { time_ns_put(p->time_ns);              p->time_ns = 0; }
    if (p->time_ns_for_children) { time_ns_put(p->time_ns_for_children); p->time_ns_for_children = 0; }

    /* Drop network-namespace reference (init_net_ns is pinned). */
    if (p->net_ns) { net_ns_put(p->net_ns); p->net_ns = 0; }

    /* Release supplementary group pages (NULL-safe, no-op if never set). */
    cred_groups_free(p);

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

    DEFINE_WAIT(wait);
    int wait_armed = 0;
    long rc = 0;
    uint64_t saved_sig_blocked = cur->sig_blocked;

    /* Linux do_wait pattern: prepare_to_wait BEFORE scanning the child set.
     * If a child's exit_notify slips in after the scan but before schedule(),
     * its wake_up(&children_wq) sees us already on the queue and flips our
     * state RUNNABLE — schedule() returns immediately, we re-scan and find
     * the new zombie. WNOHANG skips the wait machinery entirely.
     *
     * The child-signal mask is set once per syscall: SIGCHLD plus every
     * configured exit signal at entry. New children added by clone() during
     * the wait inherit through fork(); their notify_signal will be SIGCHLD
     * (default) or already in our snapshot. */
    if (!wnohang) {
        uint64_t wait_block = SIG_BIT(SIGCHLD);
        int cap0 = pid_table_capacity();
        for (int i = 1; i < cap0; i++) {
            process_t *c = pid_table[i];
            if (c && c->parent_pid == parent->pid && c->notify_signal > 0
                && c->notify_signal <= 63)
                wait_block |= SIG_BIT(c->notify_signal);
        }
        cur->sig_blocked |= wait_block;
    }

    for (;;) {
        if (!wnohang) {
            prepare_to_wait(&parent->children_wq, &wait, THREAD_BLOCKED);
            wait_armed = 1;
        }

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
                rc = child_pid; goto out;
            }

            /* WCONTINUED: report continued children */
            if (wcontinued && child->was_continued) {
                int child_pid = (int)child->pid;
                child->was_continued = 0; /* consume */
                if (wstatus) {
                    int kstatus = 0xFFFF; /* WIFCONTINUED */
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }
                rc = child_pid; goto out;
            }

            if (child->state == PROC_ZOMBIE) {
                int child_pid = (int)child->pid;
                int exit_status = child->exit_code;

                if (wstatus) {
                    int kstatus;
                    if (child->exit_signal) {
                        kstatus = child->exit_signal & 0x7F; /* killed by signal */
                        /* Linux: sig_kernel_coredump(sig) setzt 0x80 in wait-status.
                         * core-dump-erzeugende Signale: ABRT/QUIT/SEGV/BUS/FPE/ILL/
                         * XFSZ/XCPU/SYS/TRAP. */
                        switch (child->exit_signal) {
                        case SIGABRT: case SIGQUIT: case SIGSEGV: case SIGBUS:
                        case SIGFPE:  case SIGILL:  case SIGTRAP: case SIGXFSZ:
                        case SIGXCPU: case SIGSYS:
                            kstatus |= 0x80;
                            break;
                        }
                    } else {
                        kstatus = (exit_status & 0xFF) << 8;  /* normal exit */
                    }
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }

                proc_cleanup(child);
                rc = child_pid; goto out;
            }
        }

        if (!found_child) { rc = -ECHILD; goto out; }
        if (wnohang)      { rc = 0;       goto out; }

        /* Any signal outside the wait_block surfaces as ERESTARTSYS so the
         * syscall-return path can SA_RESTART or convert to -EINTR. */
        {
            uint64_t deliverable = (parent->sig_pending | cur->sig_thread_pending)
                                 & ~cur->sig_blocked;
            if (deliverable) { rc = -ERESTARTSYS; goto out; }
        }

        schedule();
    }

out:
    if (wait_armed)
        finish_wait(&parent->children_wq, &wait);
    cur->sig_blocked = saved_sig_blocked;
    return rc;
}
