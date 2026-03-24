/* CosmoRT Syscall Layer — process, thread, scheduling syscalls */

#include "internal.h"

/* ── SYS_arch_prctl (158) ────────────────────────── */

long do_arch_prctl(int code, unsigned long addr) {
    if (code == ARCH_SET_FS) {
        thread_t *t = thread_current();
        if (t) t->fs_base = addr;
        __asm__ volatile("wrmsr" :: "c"(0xC0000100),
                         "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
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
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        uint64_t val = ((uint64_t)hi << 32) | lo;
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

    /* Wake parent if blocked in wait4 */
    if (p->parent_pid) {
        process_t *parent = proc_find(p->parent_pid);
        if (parent) {
            thread_t *pt = parent->threads;
            while (pt) {
                if (pt->state == THREAD_BLOCKED) {
                    extern void sched_add(thread_t *t);
                    sched_add(pt);
                }
                pt = pt->proc_next;
            }
        }
    }
}

void do_exit(int status) {
    thread_t *t = thread_current();
    if (!t) { __asm__ volatile("cli; hlt"); return; }
    process_t *p = t->proc;
    t->state = THREAD_DEAD;

    /* CLONE_CHILD_CLEARTID: clear tid + futex_wake for pthread_join */
    if (t->clear_child_tid && p) {
        /* Ensure user page tables for user memory access */
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(p->pml4)) : "memory");
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
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
    thread_return_to_kernel(t);
    __asm__ volatile("cli; hlt");
}

void do_exit_group(int status) {
    thread_t *t = thread_current();
    if (!t) { __asm__ volatile("cli; hlt"); return; }
    process_t *p = t->proc;
    t->state = THREAD_DEAD;
    if (p) exit_kill_process(t, p, status);

    extern uint64_t pml4[];
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
    thread_return_to_kernel(t);
    __asm__ volatile("cli; hlt");
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
        uint32_t _lo, _hi;
        __asm__ volatile("rdmsr" : "=a"(_lo), "=d"(_hi) : "c"(0xC0000100));
        t->fs_base = ((uint64_t)_hi << 32) | _lo;
    }

    /* Copy parent FPU/SSE state to child thread */
    __asm__ volatile("fxsave %0" : "=m"(t->fxsave_area));

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
    struct clone_args kargs;
    kmemset(&kargs, 0, sizeof(kargs));
    size_t copy = size > sizeof(kargs) ? sizeof(kargs) : size;
    int r = copy_from_user(&kargs, uargs, copy);
    if (r) return r;

    /* Map clone3 flags to clone flags */
    unsigned long flags = (unsigned long)kargs.flags;
    void *child_stack = kargs.stack ? (void *)(kargs.stack + kargs.stack_size) : 0;
    int *parent_tid = (int *)(uintptr_t)kargs.parent_tid;
    int *child_tid = (int *)(uintptr_t)kargs.child_tid;
    unsigned long tls = (unsigned long)kargs.tls;

    return do_clone(flags, child_stack, parent_tid, child_tid, tls);
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
        if (!user_ok(a2, 1)) return -EFAULT;
        const char *uname = (const char *)a2;
        int i = 0;
        while (i < 15 && (uint64_t)(uname + i) < 0x800000000000ULL && uname[i])
            { p->comm[i] = uname[i]; i++; }
        p->comm[i] = '\0';
        return 0;
    }

    case PR_GET_NAME:
        return copy_to_user((void *)a2, p->comm, 16);

    default:
        return -EINVAL;
    }
}

/* ── SYS_statfs (137) / SYS_fstatfs (138) ───────── */

struct k_statfs {
    long f_type;     long f_bsize;
    long f_blocks;   long f_bfree;   long f_bavail;
    long f_files;    long f_ffree;
    struct { int __val[2]; } f_fsid;
    long f_namelen;  long f_frsize;
    long f_flags;    long f_spare[4];
};

static long fill_statfs(struct k_statfs *kbuf, const char *path) {
    kmemset(kbuf, 0, sizeof(*kbuf));
    kbuf->f_bsize = 4096;
    kbuf->f_frsize = 4096;
    kbuf->f_namelen = 255;

    /* Check filesystem type by path */
    int is_proc = (path && path[0]=='/' && path[1]=='p' && path[2]=='r' &&
                   path[3]=='o' && path[4]=='c' && (path[5]=='/' || path[5]==0));
    int is_tmp  = (path && path[0]=='/' && path[1]=='t' && path[2]=='m' &&
                   path[3]=='p' && (path[4]=='/' || path[4]==0));
    int is_dev  = (path && path[0]=='/' && path[1]=='d' && path[2]=='e' &&
                   path[3]=='v' && (path[4]=='/' || path[4]==0));

    if (is_proc) {
        kbuf->f_type = 0x9FA0; /* PROC_SUPER_MAGIC */
        kbuf->f_blocks = 0;
        kbuf->f_bfree = 0;
        kbuf->f_bavail = 0;
    } else if (is_tmp || is_dev) {
        kbuf->f_type = 0x01021994; /* TMPFS_MAGIC */
        kbuf->f_blocks = 1024;
        kbuf->f_bfree = 512;
        kbuf->f_bavail = 512;
    } else {
        /* CosmoFS */
        kbuf->f_type = 0x434F534D; /* 'COSM' */
        kbuf->f_blocks = (long)(page_alloc_total());
        kbuf->f_bfree = (long)(page_alloc_free());
        kbuf->f_bavail = kbuf->f_bfree;
    }
    kbuf->f_files = 1024;
    kbuf->f_ffree = 512;
    return 0;
}

long do_statfs(const char *path, void *buf) {
    char kpath[256];
    int r = copy_path_from_user(kpath, path, 256);
    if (r < 0) return r;
    struct k_statfs kbuf;
    fill_statfs(&kbuf, kpath);
    return copy_to_user(buf, &kbuf, sizeof(kbuf)) ? -EFAULT : 0;
}

long do_fstatfs(int fd, void *buf) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    struct k_statfs kbuf;
    /* Infer filesystem from fd type */
    const char *pseudo_path = "/";
    if (fde->type == FD_PROCFS) pseudo_path = "/proc";
    fill_statfs(&kbuf, pseudo_path);
    return copy_to_user(buf, &kbuf, sizeof(kbuf)) ? -EFAULT : 0;
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

/* ── SYS_sched_setaffinity (203) / getaffinity (204) ── */

long do_sched_setaffinity(int pid, size_t cpusetsize, const uint64_t *mask) {
    (void)pid; /* pid=0 → current thread */
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    uint64_t k_mask;
    int r = copy_from_user(&k_mask, mask, 8);
    if (r) return r;
    if (k_mask == 0) return -EINVAL;
    int core = __builtin_ctzll(k_mask);
    if (core >= SMP_MAX_CORES) return -EINVAL;
    t->cpu_affinity = core;
    return 0;
}

long do_sched_getaffinity(int pid, size_t cpusetsize, uint64_t *mask) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    uint64_t kmask;
    if (t->cpu_affinity >= 0)
        kmask = 1ULL << t->cpu_affinity;
    else
        kmask = ~0ULL;  /* all 64 cores */
    { int r = copy_to_user(mask, &kmask, 8); if (r) return r; }
    return (long)sizeof(uint64_t);
}

long do_sched_yield(void) {
    /* Real yield: save user state, enqueue at tail, longjmp to sched_loop.
     * sched_pick returns the next thread (FIFO round-robin), so other
     * threads (e1000d, shell) get CPU time before we run again. */
    thread_t *t = thread_current();
    if (!t) return 0;
    extern uint64_t pml4[];
    extern void sched_add(thread_t *t);
    save_user_state_for_block(t, 0); /* saves state, sets rax=0 */
    sched_add(t);                    /* enqueue at tail (FIFO) */
    t->state = THREAD_RUNNING;       /* prevent sched_loop double-add */
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
    thread_return_to_kernel(t); /* longjmp to sched_loop */
    return 0; /* unreachable */
}

/* ── SYS_sched_setscheduler (144) / getscheduler (145) ── */

struct sched_param_k { int sched_priority; };

long do_sched_setscheduler(int pid, int policy, const void *param_) {
    const struct sched_param_k *param = (const struct sched_param_k *)param_;
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (policy < 0 || policy > 2) return -EINVAL;
    t->sched_policy = policy;
    if (param) {
        struct sched_param_k kp;
        int r = copy_from_user(&kp, param, sizeof(kp));
        if (r) return r;
        if (kp.sched_priority < 0 || kp.sched_priority >= PRIO_LEVELS) return -EINVAL;
        t->priority = kp.sched_priority;
    }
    return 0;
}

long do_sched_getscheduler(int pid) {
    (void)pid;
    thread_t *t = thread_current();
    return t ? t->sched_policy : 0;
}

long do_sched_setparam(int pid, const void *param_) {
    const struct sched_param_k *param = (const struct sched_param_k *)param_;
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    struct sched_param_k kp;
    int r = copy_from_user(&kp, param, sizeof(kp));
    if (r) return r;
    if (kp.sched_priority < 0 || kp.sched_priority >= PRIO_LEVELS) return -EINVAL;
    t->priority = kp.sched_priority;
    return 0;
}

long do_sched_getparam(int pid, void *param_) {
    struct sched_param_k *param = (struct sched_param_k *)param_;
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    struct sched_param_k kparam;
    kparam.sched_priority = t->priority;
    { int r = copy_to_user(param, &kparam, sizeof(kparam)); if (r) return r; }
    return 0;
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
