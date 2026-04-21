/* CosmoRT Syscall Layer — process, thread syscalls */

#include "internal.h"

/* ── SYS_arch_prctl (158) ────────────────────────── */

long do_arch_prctl(int code, unsigned long addr) {
    if (code == ARCH_SET_FS) {
        thread_t *t = thread_current();
        if (t) t->fs_base = addr;
        hal_cpu_set_tls(addr);
        return 0;
    }
    if (code == ARCH_SET_GS) {
        /* TODO: store user GS in thread context, restore on sysret.
         * Can't use KERNEL_GS_BASE - that holds percpu after swapgs. */
        return -EINVAL;
    }
    if (code == ARCH_GET_GS) {
        /* TODO: return saved user GS-base from thread context.
         * Currently returns 0 (unset) - callers like glibc probe this. */
        uint64_t val = 0;
        int r = copy_to_user((void *)addr, &val, 8);
        if (r) return r;
        return 0;
    }
    if (code == ARCH_GET_FS) {
        uint64_t val = hal_cpu_get_tls();
        int r = copy_to_user((void *)addr, &val, 8);
        if (r) return r;
        return 0;
    }
    return -EINVAL;
}

/* ── SYS_exit (thread) / SYS_exit_group (process) ── */

static void exit_kill_process(thread_t *t, process_t *p, int status) {
    net_port_check_driver((int)p->pid);
    p->state = PROC_ZOMBIE;
    p->exit_code = status;
    /* exit_signal is set by caller for signal death, 0 for normal exit */

    /* vfork: wake blocked parent if child exits without exec */
    if (p->vfork_parent_tid) {
        thread_t *pt = thread_find_by_tid(p->vfork_parent_tid);
        if (pt) {
            extern void sched_wake(thread_t *t);
            sched_wake(pt);
        }
        p->vfork_parent_tid = 0;
    }

    /* Close all FDs immediately so pipe writers/readers see EOF */
    for (int i = 0; i < FD_MAX; i++) {
        int ftype = p->fds.entries[i].type;
        if (ftype == FD_FILE) {
            extern void vfs_file_free_obj(void *obj);
            vfs_file_free_obj(p->fds.entries[i].obj);
        } else if (ftype != FD_NONE && ftype != FD_SERIAL) {
            fd_cleanup_entry(ftype, p->fds.entries[i].obj,
                             p->fds.entries[i].flags);
        }
        p->fds.entries[i].type = FD_NONE;
        p->fds.entries[i].obj = 0;
    }
    /* Reset bitmap: all free */
    for (int w = 0; w < FD_BITMAP_WORDS; w++) p->fds.free_bitmap[w] = ~0ULL;

    /* Kill other threads.
     * Sibling threads may be in the scheduler's run queue (no way to dequeue
     * in O(1)). Set state=DEAD AND proc=NULL so sched_loop can detect and
     * skip them without dereferencing the soon-to-be-freed process struct. */
    thread_t *scan = p->threads;
    while (scan) {
        if (scan != t) {
            scan->state = THREAD_DEAD;
            scan->proc = 0;
        }
        scan = scan->proc_next;
    }

    /* Reparent children to init (pid 1) — POSIX orphan semantics.
     * Without reparenting, orphaned children can never be waited on. */
    extern process_t **pid_table;
    int cap = pid_table_capacity();
    for (int i = 1; i < cap; i++) {
        process_t *child = pid_table[i];
        if (child && child->parent_pid == p->pid) {
            child->parent_pid = 1;
            /* If child is already a zombie, notify init so it can reap */
            if (child->state == PROC_ZOMBIE) {
                process_t *init = proc_find(1);
                if (init) {
                    __sync_fetch_and_or(&init->sig_pending, SIG_BIT(SIGCHLD));
                    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
                    thread_t *it = init->threads;
                    while (it) {
                        event_post(it, 1 /* EQ_CHILD_EXITED */, (uint64_t)child->pid);
                        it = it->proc_next;
                    }
                }
            }
        }
    }

    /* Free address space and VMAs — zombie doesn't need them */
    free_address_space(p->pml4);
    p->pml4 = 0;
    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    /* Send exit signal to parent + wake if blocked in wait4 */
    if (p->parent_pid) {
        process_t *parent = proc_find(p->parent_pid);
        if (parent) {
            int nsig = p->notify_signal ? p->notify_signal : SIGCHLD;
            __sync_fetch_and_or(&parent->sig_pending, SIG_BIT(nsig));

            extern void event_post(thread_t *target, uint32_t type, uint64_t data);
            uint64_t pflags;
            spin_lock_irq(&parent->lock, &pflags);
            thread_t *pt = parent->threads;
            while (pt) {
                event_post(pt, 1 /* EQ_CHILD_EXITED */, (uint64_t)p->pid);
                pt = pt->proc_next;
            }
            spin_unlock_irq(&parent->lock, pflags);
        }
    }
}

void do_exit(int status) {
    thread_t *t = thread_current();
    if (!t) { hal_cpu_halt_noirq(); return; }
    process_t *p = t->proc;
    t->state = THREAD_DEAD;

    /* Robust Mutex Cleanup: walk userspace robust_list, mark dead-owner futexes */
    if (t->robust_list && p) {
        hal_mmu_switch(virt_to_phys(p->pml4));
        /* robust_list_head: { next_ptr, futex_offset, list_op_pending } */
        struct { void *next; long futex_offset; void *pending; } khead;
        if (!copy_from_user(&khead, t->robust_list, sizeof(khead))) {
            void *head_addr = &((struct { void *next; }*)t->robust_list)->next;
            void *entry = khead.next;
            uint32_t tid = (uint32_t)t->tid;
            int limit = 4096; /* prevent infinite loop on corrupt list */
            while (entry && entry != head_addr && --limit > 0) {
                void *next = 0;
                if (copy_from_user(&next, entry, sizeof(next))) break;
                uint32_t *futex_addr = (uint32_t *)((char *)entry + khead.futex_offset);
                if (user_ok((uint64_t)futex_addr, 4)) {
                    uint32_t fval = 0;
                    if (!copy_from_user(&fval, futex_addr, 4) &&
                        (fval & FUTEX_TID_MASK) == tid) {
                        uint32_t nval = (fval & FUTEX_WAITERS) | FUTEX_OWNER_DIED | tid;
                        __sync_val_compare_and_swap(futex_addr, fval, nval);
                        do_futex(futex_addr, 1 /* FUTEX_WAKE */, 0x7FFFFFFF, 0, 0, 0);
                    }
                }
                entry = next;
            }
            /* Also handle list_op_pending (in-progress lock at time of death) */
            if (khead.pending && user_ok((uint64_t)khead.pending, 8)) {
                uint32_t *pf = (uint32_t *)((char *)khead.pending + khead.futex_offset);
                if (user_ok((uint64_t)pf, 4)) {
                    uint32_t fval = 0;
                    if (!copy_from_user(&fval, pf, 4) &&
                        (fval & FUTEX_TID_MASK) == tid) {
                        uint32_t nval = (fval & FUTEX_WAITERS) | FUTEX_OWNER_DIED | tid;
                        __sync_val_compare_and_swap(pf, fval, nval);
                        do_futex(pf, 1, 0x7FFFFFFF, 0, 0, 0);
                    }
                }
            }
        }
        t->robust_list = 0;
    }

    /* CLONE_CHILD_CLEARTID: clear tid + futex_wake for pthread_join */
    if (t->clear_child_tid && p) {
        /* Ensure user page tables for user memory access */
        hal_mmu_switch(virt_to_phys(p->pml4));
        if (!copy_to_user(t->clear_child_tid, &(int){0}, 4)) {
            long wr = do_futex((uint32_t *)t->clear_child_tid, 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
            (void)wr;
        }
        t->clear_child_tid = 0;
    }

    if (p) {
        /* Check if last thread */
        int remaining = 0;
        thread_t *scan = p->threads;
        while (scan) {
            if (scan != t && scan->state != THREAD_DEAD) remaining++;
            scan = scan->proc_next;
        }
        if (remaining == 0) exit_kill_process(t, p, status);
    }

    extern uint64_t pml4[];
    hal_mmu_switch(virt_to_phys(pml4));
    schedule();
    __builtin_unreachable();
}

void do_exit_group(int status) {
    thread_t *t = thread_current();
    if (!t) { hal_cpu_halt_noirq(); return; }
    process_t *p = t->proc;
    t->state = THREAD_DEAD;
    if (p) exit_kill_process(t, p, status);

    extern uint64_t pml4[];
    hal_mmu_switch(virt_to_phys(pml4));
    schedule();
    __builtin_unreachable();
}

/* ── SYS_reboot (169) ────────────────────────────── */

long do_reboot(int magic1, int magic2, int cmd) {
    if ((unsigned int)magic1 != LINUX_REBOOT_MAGIC1 || (unsigned int)magic2 != LINUX_REBOOT_MAGIC2)
        return -EINVAL;
    switch (cmd) {
    case LINUX_REBOOT_CMD_POWER_OFF:
    case LINUX_REBOOT_CMD_HALT:
        serial_puts("HALT\n");
        hal_cpu_shutdown();
        hal_cpu_halt_noirq();
        return 0; /* unreachable */
    case LINUX_REBOOT_CMD_RESTART:
        hal_cpu_reset();
        return 0;
    default:
        return -EINVAL;
    }
}

/* ── SYS_clone (56) ──────────────────────────────── */

long do_clone(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls) {
    extern long kernel_clone(unsigned long, void *, int *, int *, unsigned long);
    return kernel_clone(flags, child_stack, parent_tid, child_tid, tls);
}

/* ── SYS_clone3 (435) — struct-based clone ────────── */

struct clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

long do_clone3(void *uargs, size_t size) {
    /* Linux CLONE_ARGS_SIZE_VER0 = 64 */
    if (size < 64) return -EINVAL;
    if (size > 256) return -EINVAL;
    if (!user_ok((uint64_t)uargs, size)) return -EFAULT;

    struct clone_args kargs;
    kmemset(&kargs, 0, sizeof(kargs));
    size_t copy = size > sizeof(kargs) ? sizeof(kargs) : size;
    int r = copy_from_user(&kargs, uargs, copy);
    if (r) return r;

    if (kargs.exit_signal > 64) return -EINVAL;

    /* Stack: both or neither must be set */
    if (!kargs.stack && kargs.stack_size) return -EINVAL;
    if (kargs.stack && !kargs.stack_size) return -EINVAL;
    if (kargs.stack && !user_ok(kargs.stack, kargs.stack_size)) return -EFAULT;

    /* Merge exit_signal into flags low byte (do_clone extracts it) */
    unsigned long cflags = (unsigned long)kargs.flags | (kargs.exit_signal & 0xFF);
    void *child_stack = kargs.stack ? (void *)(kargs.stack + kargs.stack_size) : 0;
    int *parent_tid = (int *)(uintptr_t)kargs.parent_tid;
    int *child_tid = (int *)(uintptr_t)kargs.child_tid;
    unsigned long tls = (unsigned long)kargs.tls;

    return do_clone(cflags, child_stack, parent_tid, child_tid, tls);
}

/* ── SYS_uname (63) ─────────────────────────────── */

struct utsname {
    char sysname[65]; char nodename[65]; char release[65];
    char version[65]; char machine[65]; char domainname[65];
};

static void kstrcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

long do_uname(void *buf_) {
    struct utsname *buf = (struct utsname *)buf_;
    struct utsname kbuf;
    kstrcpy(kbuf.sysname, "CosmoRT", 65);
    kstrcpy(kbuf.nodename, "cosmo", 65);
    kstrcpy(kbuf.release, "0.1.0", 65);
    kstrcpy(kbuf.version, "CosmoRT 0.1", 65);
    kstrcpy(kbuf.machine, "x86_64", 65);
    kstrcpy(kbuf.domainname, "", 65);
    int r = copy_to_user(buf, &kbuf, sizeof(struct utsname));
    if (r) return r;
    return 0;
}

/* ── SYS_prctl (157) ─────────────────────────────── */

long do_prctl(int option, unsigned long a2, unsigned long a3,
              unsigned long a4, unsigned long a5) {
    (void)a3; (void)a4; (void)a5;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    switch (option) {
    case PR_SET_PDEATHSIG:
        return 0; /* no-op: single-user, not critical */
    case PR_GET_PDEATHSIG:
        /* Return 0 (no parent-death signal set) */
        if (!user_ok(a2, 4)) return -EFAULT;
        return copy_to_user((void *)a2, &(int){0}, 4);

    case PR_SET_DUMPABLE:
        return 0; /* no-op: no core dumps */
    case PR_GET_DUMPABLE:
        return 1; /* always "dumpable" (SUID_DUMP_USER) */

    case PR_SET_NO_NEW_PRIVS:
        return 0; /* no-op: single-user, always unprivileged */
    case PR_GET_NO_NEW_PRIVS:
        return 0; /* not set */

    case PR_SET_NAME: {
        char kname[16];
        if (!user_ok(a2, 16)) return -EFAULT;
        int r = copy_from_user(kname, (void *)a2, 16);
        if (r) return r;
        kname[15] = '\0';
        for (int i = 0; i < 16; i++) p->comm[i] = kname[i];
        return 0;
    }

    case PR_GET_NAME:
        return copy_to_user((void *)a2, p->comm, 16);

    default:
        return -EINVAL;
    }
}

/* ── SYS_getrandom (318) ────────────────────────── */

long do_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if (!user_ok((uint64_t)buf, buflen)) return -EFAULT;
    if (buflen > 4096) buflen = 4096; /* cap like Linux */
    extern int random_get(void *buf, size_t len);
    uint8_t kbuf[256];
    size_t done = 0;
    while (done < buflen) {
        size_t chunk = buflen - done;
        if (chunk > 256) chunk = 256;
        if (random_get(kbuf, chunk) < 0) return -EIO;
        int r = copy_to_user((uint8_t *)buf + done, kbuf, chunk);
        if (r) return r;
        done += chunk;
    }
    return (long)done;
}

/* ── SYS_sysinfo (99) ────────────────────────────── */

struct k_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;       /* alignment */
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int  mem_unit;
};

long do_sysinfo(void *info_) {
    struct k_sysinfo *info = (struct k_sysinfo *)info_;
    struct k_sysinfo ksi;
    kmemset(&ksi, 0, sizeof(ksi));
    ksi.uptime = (long)(timer_ms() / 1000);
    ksi.totalram = (unsigned long)page_alloc_total() * 4096;
    ksi.freeram  = (unsigned long)page_alloc_free()  * 4096;
    /* Count live processes */
    ksi.procs = (unsigned short)proc_count_alive();
    ksi.mem_unit = 1;
    { int r = copy_to_user(info, &ksi, sizeof(ksi)); if (r) return r; }
    return 0;
}

/* ── SYS_getrusage (98) ─────────────────────────── */

struct k_timeval_ru { long tv_sec; long tv_usec; };

struct k_rusage {
    struct k_timeval_ru ru_utime;
    struct k_timeval_ru ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

long do_getrusage(int who, void *usage_) {
    struct k_rusage *usage = (struct k_rusage *)usage_;
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) return -EINVAL;
    struct k_rusage kru;
    kmemset(&kru, 0, sizeof(kru));
    if (who == RUSAGE_SELF) {
        /* ru_maxrss: peak RSS in kB. Approximate as (total - free) * 4. */
        long used_pages = (long)(page_alloc_total() - page_alloc_free());
        kru.ru_maxrss = used_pages * 4;
        /* ru_utime: approximate as total uptime (no per-process accounting yet) */
        uint64_t ms = timer_ms();
        kru.ru_utime.tv_sec = (long)(ms / 1000);
        kru.ru_utime.tv_usec = (long)((ms % 1000) * 1000);
        /* ru_stime: 0 (kernel time not tracked separately) */
    }
    { int r = copy_to_user(usage, &kru, sizeof(kru)); if (r) return r; }
    return 0;
}

/* ── SYS_prlimit64 (302) ────────────────────────── */

struct k_rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define RLIM_INFINITY  (~0UL)

long do_prlimit64(int pid, int resource,
                         const void *new_rlim_,
                         void *old_rlim_) {
    struct k_rlimit *old_rlim = (struct k_rlimit *)old_rlim_;
    const struct k_rlimit *new_rlim = (const struct k_rlimit *)new_rlim_;
    (void)pid;
    process_t *p = proc_current();
    unsigned long nofile_cur = (p && p->rlim_nofile) ? p->rlim_nofile : FD_MAX;

    unsigned long stack_cur = (p && p->rlim_stack) ? p->rlim_stack : RLIM_STACK_DEFAULT;

    /* Apply new limit */
    if (new_rlim) {
        struct k_rlimit knew;
        int r = copy_from_user(&knew, new_rlim, sizeof(knew));
        if (r) return r;
        if (resource == RLIMIT_NOFILE && p) {
            if (knew.rlim_cur > FD_MAX) knew.rlim_cur = FD_MAX;
            p->rlim_nofile = knew.rlim_cur;
            nofile_cur = knew.rlim_cur;
        }
        if (resource == RLIMIT_STACK && p) {
            if (knew.rlim_cur > RLIM_STACK_MAX) knew.rlim_cur = RLIM_STACK_MAX;
            p->rlim_stack = knew.rlim_cur;
            stack_cur = knew.rlim_cur;
        }
        if (resource == RLIMIT_DATA && p) {
            /* 0 or RLIM_INFINITY = unlimited (Linux default) */
            p->rlim_data = (knew.rlim_cur == RLIM_INFINITY) ? 0 : knew.rlim_cur;
        }
    }

    if (old_rlim) {
        struct k_rlimit krl;
        switch (resource) {
        case RLIMIT_DATA:
            krl.rlim_cur = (p && p->rlim_data) ? p->rlim_data : RLIM_INFINITY;
            krl.rlim_max = RLIM_INFINITY;
            break;
        case RLIMIT_STACK:
            krl.rlim_cur = stack_cur;
            krl.rlim_max = RLIM_STACK_MAX;
            break;
        case RLIMIT_NOFILE:
            krl.rlim_cur = nofile_cur;
            krl.rlim_max = FD_MAX;
            break;
        case RLIMIT_AS:
            krl.rlim_cur = RLIM_INFINITY;
            krl.rlim_max = RLIM_INFINITY;
            break;
        default:
            krl.rlim_cur = RLIM_INFINITY;
            krl.rlim_max = RLIM_INFINITY;
            break;
        }
        int r = copy_to_user(old_rlim, &krl, sizeof(krl));
        if (r) return r;
    }
    return 0;
}

/* ── SYS_times (100) ────────────────────────────── */

struct k_tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

long do_times(void *buf_) {
    struct k_tms *buf = (struct k_tms *)buf_;
    struct k_tms ktms;
    /* CLK_TCK = 100 Hz on Linux x86_64 */
    uint64_t ticks = timer_ms() / 10;
    ktms.tms_utime  = (long)ticks; /* approximate: all time as utime */
    ktms.tms_stime  = 0;
    ktms.tms_cutime = 0;           /* children time not tracked */
    ktms.tms_cstime = 0;
    { int r = copy_to_user(buf, &ktms, sizeof(ktms)); if (r) return r; }
    return (long)ticks;
}

/* ── SYS_getcpu (309) ────────────────────────────── */

/* ── SYS_pause (34) — block until signal ────────── */

long do_pause(void) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    /* Block indefinitely — returns on signal delivery */
    event_t ev;
    event_wait(&t->eq, &ev, -1);
    return -EINTR;
}

/* ── SYS_setitimer (38) / SYS_getitimer (36) ─────── */

struct k_itimerval {
    struct k_timeval it_interval;
    struct k_timeval it_value;
};

long do_getitimer(int which, void *curr_value) {
    if (which != 0) return -EINVAL; /* only ITIMER_REAL */
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    struct k_itimerval kval;
    kmemset(&kval, 0, sizeof(kval));
    if (p->alarm_deadline_ms > 0) {
        uint64_t now = timer_ms();
        if (p->alarm_deadline_ms > now) {
            uint64_t rem_ms = p->alarm_deadline_ms - now;
            kval.it_value.tv_sec = (long)(rem_ms / 1000);
            kval.it_value.tv_usec = (long)((rem_ms % 1000) * 1000);
        }
    }
    return copy_to_user(curr_value, &kval, sizeof(kval));
}

long do_setitimer(int which, const void *new_value, void *old_value) {
    if (which != 0) return -EINVAL; /* only ITIMER_REAL */
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    /* Return old value first */
    if (old_value) {
        long r = do_getitimer(0, old_value);
        if (r) return r;
    }

    if (new_value) {
        struct k_itimerval kval;
        int r = copy_from_user(&kval, new_value, sizeof(kval));
        if (r) return r;
        uint64_t ms = (uint64_t)kval.it_value.tv_sec * 1000 +
                      (uint64_t)kval.it_value.tv_usec / 1000;
        if (ms == 0)
            p->alarm_deadline_ms = 0; /* cancel */
        else
            p->alarm_deadline_ms = timer_ms() + ms;
        /* Note: it_interval is ignored — we don't support repeating timers.
         * alarm(2) and most callers only use one-shot ITIMER_REAL. */
    }

    return 0;
}

/* ── SYS_waitid (247) — wait for process ──────────── */

/* idtype values */
#define P_PID  1
#define P_PGID 2
#define P_ALL  0

/* siginfo_t layout (simplified for waitid): 128 bytes total */
struct k_siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    int _pad0;
    int si_pid;
    int si_uid;
    int si_status;
    int _pad1;
    /* rest is padding to 128 bytes */
};

/* CLD_* codes */
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

long do_waitid(int idtype, int id, void *infop, int options) {
    /* Convert to wait4 semantics */
    int wait4_pid;
    switch (idtype) {
    case P_PID:  wait4_pid = id; break;
    case P_PGID: wait4_pid = -id; break;
    case P_ALL:  wait4_pid = -1; break;
    default: return -EINVAL;
    }

    int wstatus = 0;
    long ret = do_wait4(wait4_pid, &wstatus, options, 0);
    if (ret < 0) return ret;
    if (ret == 0) {
        /* WNOHANG and no child ready: clear infop.si_pid */
        if (infop) {
            struct k_siginfo ksi;
            kmemset(&ksi, 0, sizeof(ksi));
            copy_to_user(infop, &ksi, sizeof(ksi));
        }
        return 0;
    }

    if (infop) {
        struct k_siginfo ksi;
        kmemset(&ksi, 0, sizeof(ksi));
        ksi.si_signo = SIGCHLD;
        ksi.si_pid = (int)ret;
        ksi.si_uid = 0;

        if ((wstatus & 0x7f) == 0) {
            /* Normal exit: WIFEXITED */
            ksi.si_code = CLD_EXITED;
            ksi.si_status = (wstatus >> 8) & 0xff;
        } else if ((wstatus & 0x7f) != 0x7f) {
            /* Killed by signal: WIFSIGNALED */
            ksi.si_code = CLD_KILLED;
            ksi.si_status = wstatus & 0x7f;
        } else if ((wstatus >> 8) != 0) {
            /* Stopped: WIFSTOPPED */
            ksi.si_code = CLD_STOPPED;
            ksi.si_status = (wstatus >> 8) & 0xff;
        }

        copy_to_user(infop, &ksi, sizeof(ksi));
    }

    return 0; /* waitid returns 0 on success, not pid */
}

/* ── SYS_getcpu (309) ────────────────────────────── */

long do_getcpu(unsigned *cpu_ptr, unsigned *node_ptr) {
    unsigned c = (unsigned)percpu_self()->core_id;
    if (cpu_ptr && user_ok((uint64_t)cpu_ptr, 4))
        copy_to_user(cpu_ptr, &c, 4);
    if (node_ptr && user_ok((uint64_t)node_ptr, 4)) {
        unsigned zero = 0;
        copy_to_user(node_ptr, &zero, 4);
    }
    return 0;
}
