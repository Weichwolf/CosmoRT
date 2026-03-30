/* CosmoRT Process — wait4, process cleanup */

#include "proc/proc_internal.h"

void proc_cleanup(process_t *p) {
    if (!p) return;

    extern void flock_release_pid(uint32_t pid);
    flock_release_pid(p->pid);

    for (int i = 0; i < FD_MAX; i++) {
        int type = p->fds.entries[i].type;
        if (type == FD_FILE) {
            vfs_file_free_obj(p->fds.entries[i].obj);
        } else if (type != FD_NONE && type != FD_SERIAL) {
            fd_cleanup_entry(type, p->fds.entries[i].obj);
        }
        p->fds.entries[i].type = FD_NONE;
        p->fds.entries[i].obj = 0;
    }
    for (int w = 0; w < FD_BITMAP_WORDS; w++) p->fds.free_bitmap[w] = ~0ULL;

    thread_t *t = p->threads;
    while (t) {
        thread_t *next = t->proc_next;
        if (t->proc == p) {
            t->state = THREAD_DEAD;
            thread_free(t);
        }
        t = next;
    }
    p->threads = 0;
    p->main_thread = 0;
    p->thread_count = 0;

    unmap_shared_vmas(p->vma_root, p->pml4);

    free_address_space(p->pml4);
    p->pml4 = 0;

    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    if (p->pid < PID_TABLE_MAX)
        pid_table[p->pid] = 0;

    slab_free(&proc_slab, p);
}

#define SA_NOCLDWAIT_VAL 0x00000002

long do_wait4(int pid, int *wstatus, int options, void *rusage) {
    (void)rusage;

    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *parent = cur->proc;

    if ((parent->sig_actions[17 ].sa_flags & SA_NOCLDWAIT_VAL) &&
        parent->sig_actions[17].sa_handler == (void *)0) {
        for (int i = 1; i < PID_TABLE_MAX; i++) {
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

    int wnohang    = options & 1;
    int wuntraced  = options & 2;
    int wcontinued = options & 8;

    for (;;) {
        int found_child = 0;

        for (int i = 1; i < PID_TABLE_MAX; i++) {
            process_t *child = pid_table[i];
            if (!child || child->state == PROC_FREE) continue;
            if (child->pid != (uint32_t)i) continue;
            if (child->parent_pid != parent->pid) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;
            if (pid == 0 || pid == -1) { }

            found_child = 1;

            if (wuntraced && child->stop_signal) {
                int child_pid = (int)child->pid;
                int stop_sig = child->stop_signal;
                child->stop_signal = 0;
                if (wstatus) {
                    int kstatus = (stop_sig << 8) | 0x7F;
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }
                return child_pid;
            }

            if (wcontinued && child->was_continued) {
                int child_pid = (int)child->pid;
                child->was_continued = 0;
                if (wstatus) {
                    int kstatus = 0xFFFF;
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }
                return child_pid;
            }

            if (child->state == PROC_ZOMBIE) {
                int child_pid = (int)child->pid;
                int exit_status = child->exit_code;

                if (wstatus) {
                    int kstatus;
                    if (child->exit_signal) {
                        int sig = child->exit_signal & 0x7F;
                        kstatus = sig;
                        if (sig == 3 || sig == 4 || sig == 5 || sig == 6 ||
                            sig == 7 || sig == 8 || sig == 11 || sig == 31)
                            kstatus |= 0x80;
                    }
                    else
                        kstatus = (exit_status & 0xFF) << 8;
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }

                proc_cleanup(child);
                return child_pid;
            }
        }

        if (!found_child) return -ECHILD;
        if (wnohang) return 0;

        {
            event_t ev;
            int _wr = event_wait(&cur->eq, &ev, -1);
            if (_wr == -4) return -EINTR;
        }
    }
}
