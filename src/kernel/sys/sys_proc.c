/* CosmoRT Syscall Layer — process, thread syscalls */

#include "internal.h"

/* ── SYS_arch_prctl (158) ────────────────────────── */

long do_arch_prctl(int code, unsigned long addr) {
    if (code == ARCH_SET_FS) {
        thread_t *t = thread_current();
        if (t) t->fs_base = addr;
        arch_set_fs_base(addr);
        return 0;
    }
    if (code == ARCH_SET_GS) {
        /* User GS: write to IA32_KERNEL_GS_BASE so next swapgs restores it.
         * No — that would clobber our percpu pointer!
         * User GS must be stored in thread context and restored on sysret.
         * Known limitation: GS not actively used by CosmoRT (TLS uses FS). */
        return -EINVAL;
    }
    if (code == ARCH_GET_GS) {
        /* Known limitation: CosmoRT doesn't manage user GS-base (TLS uses FS).
         * After SWAPGS in syscall entry, KERNEL_GS_BASE holds user's GS-base,
         * but we never set it, so it's always 0.  Return 0 to indicate "unset"
         * rather than -EINVAL, since callers (e.g. glibc) may probe this. */
        uint64_t val = 0;
        int r = copy_to_user((void *)addr, &val, 8);
        if (r) return r;
        return 0;
    }
    if (code == ARCH_GET_FS) {
        uint64_t val = arch_get_fs_base();
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

    /* Close all FDs immediately so pipe writers/readers see EOF */
    for (int i = 0; i < FD_MAX; i++) {
        int ftype = p->fds.entries[i].type;
        if (ftype == FD_FILE) {
            extern void vfs_file_free_obj(void *obj);
            vfs_file_free_obj(p->fds.entries[i].obj);
        } else if (ftype != FD_NONE && ftype != FD_SERIAL) {
            fd_cleanup_entry(ftype, p->fds.entries[i].obj);
        }
        p->fds.entries[i].type = FD_NONE;
        p->fds.entries[i].obj = 0;
    }
    /* Reset bitmap: all free */
    for (int w = 0; w < FD_BITMAP_WORDS; w++) p->fds.free_bitmap[w] = ~0ULL;

    /* Kill other threads */
    thread_t *scan = p->threads;
    while (scan) {
        if (scan != t) scan->state = THREAD_DEAD;
        scan = scan->proc_next;
    }

    /* Free address space and VMAs — zombie doesn't need them */
    free_address_space(p->pml4);
    p->pml4 = 0;
    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    /* Send SIGCHLD to parent + wake if blocked in wait4 */
    if (p->parent_pid) {
        process_t *parent = proc_find(p->parent_pid);
        if (parent) {
            /* SIGCHLD delivery (Linux ABI) */
            __sync_fetch_and_or(&parent->sig_pending, 1ULL << SIGCHLD);

            extern void sched_add(thread_t *t);
            uint64_t pflags;
            spin_lock_irq(&parent->lock, &pflags);
            thread_t *pt = parent->threads;
            while (pt) {
                if (pt->state == THREAD_BLOCKED) {
                    pt->state = THREAD_RUNNING;
                    sched_add(pt);
                }
                pt = pt->proc_next;
            }
            spin_unlock_irq(&parent->lock, pflags);
        }
    }
}

void do_exit(int status) {
    thread_t *t = thread_current();
    if (!t) { arch_cli_halt(); return; }
    process_t *p = t->proc;
    t->state = THREAD_DEAD;

    /* CLONE_CHILD_CLEARTID: clear tid + futex_wake for pthread_join */
    if (t->clear_child_tid && p) {
        /* Ensure user page tables for user memory access */
        arch_set_cr3(virt_to_phys(p->pml4));
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
    arch_set_cr3(virt_to_phys(pml4));
    thread_return_to_kernel(t);
    arch_cli_halt();
}

void do_exit_group(int status) {
    thread_t *t = thread_current();
    if (!t) { arch_cli_halt(); return; }
    process_t *p = t->proc;
    t->state = THREAD_DEAD;
    if (p) exit_kill_process(t, p, status);

    extern uint64_t pml4[];
    arch_set_cr3(virt_to_phys(pml4));
    thread_return_to_kernel(t);
    arch_cli_halt();
}

/* ── SYS_reboot (169) ────────────────────────────── */

long do_reboot(int magic1, int magic2, int cmd) {
    if ((unsigned int)magic1 != LINUX_REBOOT_MAGIC1 || (unsigned int)magic2 != LINUX_REBOOT_MAGIC2)
        return -EINVAL;
    switch (cmd) {
    case LINUX_REBOOT_CMD_POWER_OFF:
    case LINUX_REBOOT_CMD_HALT:
        serial_puts("HALT\n");
        arch_shutdown();
        arch_cli_halt();
        return 0; /* unreachable */
    case LINUX_REBOOT_CMD_RESTART:
        /* Triple fault → CPU reset */
        arch_cli();
        __asm__ volatile("lidt %0" :: "m"((struct { uint16_t l; uint64_t b; }){0, 0}));
        __asm__ volatile("int3");
        arch_cli_halt();
        return 0;
    default:
        return -EINVAL;
    }
}

/* ── SYS_clone (56) ──────────────────────────────── */

long do_clone(unsigned long flags, void *child_stack,
                     int *parent_tid, int *child_tid, unsigned long tls) {
    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || !cur->proc) return -EFAULT;

    /* CLONE_VM required — CosmoRT only supports in-process threads.
     * CLONE_FS, CLONE_FILES, CLONE_SIGHAND: with CLONE_VM, child shares
     * the same process_t → CWD, FD table, signal handlers are already
     * shared implicitly. Accept and document.
     * CLONE_SYSVSEM: no SysV semaphores in CosmoRT. Accept and ignore. */
    if (!(flags & CLONE_VM)) return -EINVAL; /* fork not supported */

    /* Read parent's saved user registers from the syscall frame */
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;

    /* Allocate new thread */
    thread_t *t = thread_alloc();
    if (!t) return -ENOMEM;

    /* Copy parent's register state */
    t->rip    = frame->rcx;    /* user RIP (SYSCALL saved in RCX) */
    t->rflags = frame->r11;    /* user RFLAGS (SYSCALL saved in R11) */
    t->rax    = 0;             /* clone() returns 0 in child */
    t->rbx    = frame->rbx;
    t->rcx    = frame->rcx;
    t->rdx    = frame->rdx;
    t->rsi    = frame->rsi;
    t->rdi    = frame->rdi;
    t->rbp    = frame->rbp;
    t->r8     = frame->r8;
    t->r9     = frame->r9;
    t->r10    = frame->r10;
    t->r11    = frame->r11;
    t->r12    = frame->r12;
    t->r13    = frame->r13;
    t->r14    = frame->r14;
    t->r15    = frame->r15;

    /* Child stack */
    t->rsp = child_stack ? (uint64_t)child_stack : frame->rsi; /* RSI had arg2 */

    /* Share address space (CLONE_VM) */
    t->proc = cur->proc;
    t->state = THREAD_RUNNABLE;
    t->sig_blocked = cur->sig_blocked; /* inherit parent thread's signal mask */
    t->sched_policy = cur->sched_policy;
    t->priority = cur->priority;
    t->cpu_affinity = -1;
    t->timeslice = RR_TIMESLICE;

    /* Kernel stack for new thread */
    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) { thread_free(t); return -ENOMEM; }
    t->kstack_top = (uint64_t)(uintptr_t)(t->kstack + KSTACK_SIZE);

    /* TLS (CLONE_SETTLS) */
    if (flags & CLONE_SETTLS) {
        t->fs_base = tls;
    } else {
        /* Read live FS_BASE from MSR — cur->fs_base may be stale */
        t->fs_base = arch_get_fs_base();
    }

    /* Copy parent FPU/SSE state to child thread */
    arch_fxsave(t->fxsave_area);

    /* Parent TID (CLONE_PARENT_SETTID) */
    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        if (!user_ok((uint64_t)parent_tid, 4)) { thread_free(t); return -EFAULT; }
        *parent_tid = t->tid;
    }

    /* Child TID (CLONE_CHILD_SETTID) */
    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        if (!user_ok((uint64_t)child_tid, 4)) { thread_free(t); return -EFAULT; }
        *child_tid = t->tid;
    }

    /* CLONE_CHILD_CLEARTID: on thread exit, clear *child_tid to 0 and futex_wake */
    t->clear_child_tid = 0;
    if ((flags & CLONE_CHILD_CLEARTID) && child_tid)
        t->clear_child_tid = child_tid;

    /* Add to process thread list (under lock for concurrent clone safety) */
    {
        uint64_t lflags;
        spin_lock_irq(&cur->proc->lock, &lflags);
        t->proc_next = cur->proc->threads;
        cur->proc->threads = t;
        cur->proc->thread_count++;
        spin_unlock_irq(&cur->proc->lock, lflags);
    }

    /* Add to scheduler */
    extern void sched_add(thread_t *t);
    sched_add(t);

    return (long)t->tid;
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
    /* Minimum: must include at least up to exit_signal (first 5 fields = 40 bytes) */
    if (size < __builtin_offsetof(struct clone_args, stack)) return -EINVAL;
    if (size > 256) return -EINVAL;
    if (!user_ok((uint64_t)uargs, size)) return -EFAULT;

    struct clone_args kargs;
    kmemset(&kargs, 0, sizeof(kargs));
    size_t copy = size > sizeof(kargs) ? sizeof(kargs) : size;
    int r = copy_from_user(&kargs, uargs, copy);
    if (r) return r;

    /* Validate copied args */
    if (kargs.exit_signal > 64) return -EINVAL;
    if (kargs.stack && !user_ok(kargs.stack, kargs.stack_size)) return -EFAULT;
    if (kargs.stack && kargs.stack_size == 0) return -EINVAL;

    /* Map clone3 flags to clone flags */
    unsigned long cflags = (unsigned long)kargs.flags;
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
    unsigned short nprocs = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_pool[i].state == PROC_ALIVE) nprocs++;
    ksi.procs = nprocs;
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

#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define RLIM_INFINITY  (~0UL)

long do_prlimit64(int pid, int resource,
                         const void *new_rlim_,
                         void *old_rlim_) {
    struct k_rlimit *old_rlim = (struct k_rlimit *)old_rlim_;
    (void)pid; (void)new_rlim_; /* ignore set for now */
    if (old_rlim) {
        struct k_rlimit krl;
        switch (resource) {
        case RLIMIT_STACK:
            krl.rlim_cur = 8 * 1024 * 1024;      /* 8 MB */
            krl.rlim_max = 64 * 1024 * 1024;     /* 64 MB */
            break;
        case RLIMIT_NOFILE:
            krl.rlim_cur = FD_MAX;
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
