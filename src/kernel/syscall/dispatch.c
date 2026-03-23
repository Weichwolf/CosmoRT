/* CosmoRT Syscall Dispatcher — routes syscall numbers to handlers */

#include "internal.h"

/* Copy user path string to kernel buffer with full bounds checking.
 * Returns string length (excluding NUL) or negative errno. */
int copy_path_from_user(char *kbuf, const char *upath, size_t max) {
    if (!user_ok((uint64_t)upath, 1)) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
        if ((uint64_t)(upath + i) >= 0x800000000000ULL) return -EFAULT;
        kbuf[i] = upath[i];
        if (kbuf[i] == '\0') return (int)i;
    }
    return -ENAMETOOLONG;
}

/* Save user register state from syscall frame into thread_t.
 * Used by clone, futex_wait, and any syscall that blocks. */
void save_user_state_for_block(thread_t *t, long return_value) {
    percpu_t *cpu = percpu_self();
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    t->rip    = frame->rcx;       /* user RIP */
    t->rflags = frame->r11;       /* user RFLAGS */
    t->rsp    = cpu->user_rsp;    /* user RSP */
    t->rax    = (uint64_t)return_value;
    t->rbx = frame->rbx; t->rcx = frame->rcx; t->rdx = frame->rdx;
    t->rsi = frame->rsi; t->rdi = frame->rdi; t->rbp = frame->rbp;
    t->r8  = frame->r8;  t->r9  = frame->r9;  t->r10 = frame->r10;
    t->r11 = frame->r11; t->r12 = frame->r12; t->r13 = frame->r13;
    t->r14 = frame->r14; t->r15 = frame->r15;
}

/* ── Dispatcher ──────────────────────────────────── */

static long sys_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    switch (num) {
    /* I/O */
    case SYS_READ:          return do_read((int)a1, (void *)a2, (size_t)a3);
    case SYS_WRITE:         return do_write((int)a1, (const void *)a2, (size_t)a3);
    case SYS_WRITEV:        return do_writev((int)a1, (const struct iovec *)a2, (int)a3);
    case SYS_CLOSE:         return do_close((int)a1);

    /* Memory */
    case SYS_BRK:           return do_brk((unsigned long)a1);
    case SYS_MMAP:          return do_mmap((unsigned long)a1, (size_t)a2, (int)a3,
                                           (int)a4, (int)a5, a6);
    case SYS_MUNMAP:        return do_munmap((unsigned long)a1, (size_t)a2);
    case SYS_MPROTECT:      return do_mprotect((unsigned long)a1, (size_t)a2, (int)a3);
    case SYS_MLOCK:         return do_mlock((unsigned long)a1, (size_t)a2);
    case SYS_MUNLOCK:       return do_munlock((unsigned long)a1, (size_t)a2);
    case SYS_MLOCKALL:      return do_mlockall((int)a1);
    case SYS_MUNLOCKALL:    return do_munlockall();

    /* Process lifecycle */
    case SYS_EXIT:          do_exit((int)a1); return 0;
    case SYS_EXIT_GROUP:    do_exit_group((int)a1); return 0;
    case SYS_CLONE:         return do_clone((unsigned long)a1, (void *)a2,
                                            (int *)a3, (int *)a4, (unsigned long)a5);
    case SYS_FORK:          return do_fork();
    case SYS_EXECVE:        return do_execve((const char *)a1,
                                             (char *const *)a2, (char *const *)a3);
    case SYS_WAIT4:         return do_wait4((int)a1, (int *)a2, (int)a3, (void *)a4);

    /* Thread/TLS */
    case SYS_ARCH_PRCTL:    return do_arch_prctl((int)a1, (unsigned long)a2);
    case SYS_SET_TID_ADDRESS: {
        thread_t *t = thread_current();
        return t ? (long)t->tid : 1;
    }
    case SYS_SET_ROBUST_LIST: return 0;

    /* Signals */
    case SYS_RT_SIGACTION:    return do_rt_sigaction((int)a1,
                                       (const void *)a2,
                                       (void *)a3, (size_t)a4);
    case SYS_RT_SIGPROCMASK:  return do_rt_sigprocmask((int)a1,
                                       (const uint64_t *)a2, (uint64_t *)a3, (size_t)a4);
    case SYS_RT_SIGRETURN:    return do_rt_sigreturn();
    case SYS_KILL:            return do_kill((int)a1, (int)a2);

    /* Identity */
    case SYS_GETPID:  { process_t *p = proc_current(); return p ? (long)p->pid : 1; }
    case SYS_GETPPID: { process_t *p = proc_current(); return p ? (long)p->parent_pid : 0; }
    case SYS_GETTID:  { thread_t *t = thread_current(); return t ? (long)t->tid : 1; }
    case SYS_GETUID:  return 0;
    case SYS_GETGID:  return 0;
    case SYS_GETEUID: return 0;
    case SYS_GETEGID: return 0;

    /* Process groups / sessions */
    case SYS_SETPGID: {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        int target_pid = (int)a1;
        int new_pgid   = (int)a2;
        process_t *target = (target_pid == 0) ? p : proc_find((uint32_t)target_pid);
        if (!target || target->state != PROC_ALIVE) return -ESRCH;
        /* Can only setpgid on self or own child */
        if (target != p && target->parent_pid != p->pid) return -ESRCH;
        /* Must be in same session */
        if (target->sid != p->sid) return -EPERM;
        target->pgid = (new_pgid == 0) ? target->pid : (uint32_t)new_pgid;
        return 0;
    }
    case SYS_GETPGRP: {
        process_t *p = proc_current();
        return p ? (long)p->pgid : 1;
    }
    case SYS_GETPGID: {
        int target_pid = (int)a1;
        process_t *p;
        if (target_pid == 0) {
            p = proc_current();
        } else {
            p = proc_find((uint32_t)target_pid);
        }
        if (!p || p->state == PROC_FREE) return -ESRCH;
        return (long)p->pgid;
    }
    case SYS_SETSID: {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        /* Already a process group leader → EPERM */
        if (p->pgid == p->pid) {
            /* Check if any other process shares this pgid (would make us a PG leader).
             * Simplified: allow setsid always since we're single-user and
             * the common case is shell pipelines calling setsid(). */
        }
        p->sid  = p->pid;
        p->pgid = p->pid;
        return (long)p->pid;
    }
    case SYS_GETSID: {
        int target_pid = (int)a1;
        process_t *p;
        if (target_pid == 0) {
            p = proc_current();
        } else {
            p = proc_find((uint32_t)target_pid);
        }
        if (!p || p->state == PROC_FREE) return -ESRCH;
        return (long)p->sid;
    }
    case SYS_PRCTL:       return -ENOSYS;
    case SYS_SIGALTSTACK: return 0; /* accept but ignore */
    case SYS_RT_SIGSUSPEND: return do_rt_sigsuspend((const uint64_t *)a1, (size_t)a2);
    case SYS_TGKILL:      return do_tgkill((int)a1, (int)a2, (int)a3);
    case SYS_GETRLIMIT:   return do_prlimit64(0, (int)a1, 0, (void *)a2);
    case SYS_DUP: {
        /* dup(oldfd): find lowest free fd */
        process_t *dp = proc_current();
        if (!dp) return -EFAULT;
        fd_entry_t *dold = fd_get(&dp->fds, (int)a1);
        if (!dold) return -EBADF;
        for (int di = 0; di < FD_MAX; di++) {
            if (dp->fds.entries[di].type == FD_NONE) {
                dp->fds.entries[di] = *dold;
                if (dold->type == FD_FILE && dold->obj) {
                    extern void vfs_file_incref(struct vfs_file *f);
                    vfs_file_incref((struct vfs_file *)dold->obj);
                } else if (dold->type == FD_PIPE && dold->obj) {
                    fd_obj_incref(FD_PIPE, dold->obj);
                } else if (dold->type == FD_UNIX_SOCK && dold->obj) {
                    usock_incref(dold->obj);
                }
                if (di >= dp->fds.max_fd) dp->fds.max_fd = di + 1;
                return di;
            }
        }
        return -EMFILE;
    }
    case SYS_VFORK:       return do_fork(); /* vfork = fork (no COW optimization) */
    case SYS_CLONE3:      return do_clone3((void *)a1, (size_t)a2);
    case SYS_MOUNT:       return 0; /* pretend mount succeeds */
    case SYS_SETHOSTNAME: return 0;
    case SYS_STATFS:      return -ENOSYS;
    case SYS_FSTATFS:     return -ENOSYS;
    case SYS_ACCEPT4: {
        process_t *a4p = proc_current();
        if (a4p) {
            fd_entry_t *a4e = fd_get(&a4p->fds, (int)a1);
            if (a4e && a4e->type == FD_UNIX_SOCK)
                return usock_accept4((int)a1, (void *)a2, (int *)a3, (int)a4);
        }
        return do_accept((int)a1, (void *)a2, (int *)a3);
    }
    case SYS_MREMAP:      return do_mremap((unsigned long)a1, (size_t)a2,
                                           (size_t)a3, (int)a4, (unsigned long)a5);
    case SYS_MADVISE:     return do_madvise((unsigned long)a1, (size_t)a2, (int)a3);
    case SYS_FACCESSAT:   return do_access((const char *)a2); /* path in a2 (dirfd ignored) */
    case SYS_READLINKAT:  return do_readlink((const char *)a2, (char *)a3, (size_t)a4);

    /* System info */
    case SYS_UNAME:     return do_uname((void *)a1);
    case SYS_GETRANDOM: return do_getrandom((void *)a1, (size_t)a2, (unsigned int)a3);
    case SYS_PRLIMIT64: return do_prlimit64((int)a1, (int)a2,
                                           (const void *)a3, (void *)a4);
    case SYS_SYSINFO:   return do_sysinfo((void *)a1);
    case SYS_GETRUSAGE: return do_getrusage((int)a1, (void *)a2);
    case SYS_TIMES:     return do_times((void *)a1);
    case SYS_RSEQ:      return -ENOSYS;
    case SYS_CAPGET:    return -EPERM;
    case SYS_CAPSET:    return -EPERM;
    case SYS_TIME: {
        /* time(2): return seconds since epoch */
        extern uint64_t timer_ms(void);
        extern uint64_t rtc_epoch_sec;
        long secs = (long)(timer_ms() / 1000 + rtc_epoch_sec);
        if (a1 && user_ok((uint64_t)a1, 8))
            kmemcpy((void *)a1, &secs, sizeof(secs));
        return secs;
    }

    /* Timers / clocks */
    case SYS_CLOCK_GETTIME:   return do_clock_gettime((int)a1, (void *)a2);
    case SYS_CLOCK_GETRES:    return do_clock_getres((int)a1, (void *)a2);
    case SYS_CLOCK_NANOSLEEP: return do_clock_nanosleep((int)a1, (int)a2,
                                       (const void *)a3, (void *)a4);
    case SYS_NANOSLEEP:       return do_nanosleep((const void *)a1, (void *)a2);
    case SYS_GETTIMEOFDAY:    return do_gettimeofday((void *)a1, (void *)a2);

    /* Scheduling */
    case SYS_SCHED_SETAFFINITY: return do_sched_setaffinity((int)a1, (size_t)a2, (const uint64_t *)a3);
    case SYS_SCHED_GETAFFINITY: return do_sched_getaffinity((int)a1, (size_t)a2, (uint64_t *)a3);
    case SYS_SCHED_YIELD:       return do_sched_yield();
    case SYS_SCHED_SETSCHEDULER: return do_sched_setscheduler((int)a1, (int)a2, (const void *)a3);
    case SYS_SCHED_GETSCHEDULER: return do_sched_getscheduler((int)a1);
    case SYS_SCHED_SETPARAM:    return do_sched_setparam((int)a1, (const void *)a2);
    case SYS_SCHED_GETPARAM:    return do_sched_getparam((int)a1, (void *)a2);

    /* Futex */
    case SYS_FUTEX:
        if (!user_ok((uint64_t)a1, 4)) return -EFAULT;
        return do_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                                        (const struct timespec *)a4,
                                        (uint32_t *)a5, (uint32_t)a6);

    /* Filesystem */
    case SYS_OPEN:   return do_open((const char *)a1, (int)a2, (int)a3);
    case SYS_OPENAT: return do_openat((int)a1, (const char *)a2, (int)a3, (int)a4);
    case SYS_LSEEK:  return do_lseek((int)a1, a2, (int)a3);
    case SYS_FSTAT:  return do_fstat((int)a1, (struct k_stat *)a2);
    case SYS_STAT:   return do_stat((const char *)a1, (struct k_stat *)a2);
    case SYS_LSTAT:  return do_lstat((const char *)a1, (struct k_stat *)a2);
    case SYS_FSTATAT: return do_fstatat((int)a1, (const char *)a2,
                                         (struct k_stat *)a3, (int)a4);
    case SYS_DUP2:   return do_dup2((int)a1, (int)a2);
    case SYS_DUP3:   return do_dup3((int)a1, (int)a2, (int)a3);
    case SYS_GETCWD: return do_getcwd((char *)a1, (size_t)a2);
    case SYS_CHDIR:  return do_chdir((const char *)a1);

    /* Network / sockets */
    case SYS_SOCKET:      return do_socket((int)a1, (int)a2, (int)a3);
    case SYS_CONNECT:     return do_connect((int)a1, (const void *)a2, (int)a3);
    case SYS_BIND:        return do_bind((int)a1, (const void *)a2, (int)a3);
    case SYS_LISTEN:      return do_listen((int)a1, (int)a2);
    case SYS_ACCEPT:      return do_accept((int)a1, (void *)a2, (int *)a3);
    case SYS_SENDTO:      return do_sendto((int)a1, (const void *)a2, a3, (int)a4,
                                           (const void *)a5, (int)a6);
    case SYS_RECVFROM:    return do_recvfrom((int)a1, (void *)a2, a3, (int)a4,
                                             (void *)a5, (int *)a6);
    case SYS_SETSOCKOPT:  return do_setsockopt((int)a1, (int)a2, (int)a3,
                                               (const void *)a4, (int)a5);
    case SYS_GETSOCKOPT:  return do_getsockopt((int)a1, (int)a2, (int)a3,
                                               (void *)a4, (int *)a5);
    case SYS_GETSOCKNAME: return do_getsockname((int)a1, (void *)a2, (int *)a3);
    case SYS_GETPEERNAME: return do_getpeername((int)a1, (void *)a2, (int *)a3);
    case SYS_SENDMSG:     return usock_sendmsg((int)a1, (const void *)a2, (int)a3);
    case SYS_RECVMSG:     return usock_recvmsg((int)a1, (void *)a2, (int)a3);
    case SYS_SHUTDOWN:     return 0;
    case SYS_SOCKETPAIR: {
        if ((int)a1 != 1 /* AF_UNIX */) return -EAFNOSUPPORT;
        return usock_socketpair((int)a2, (int *)a4);
    }
    case SYS_POLL:        return do_poll((void *)a1, (int)a2, (int)a3);

    /* Filesystem mutation */
    case SYS_MKDIR:      return do_mkdir((const char *)a1, (int)a2);
    case SYS_MKDIRAT:    return do_mkdirat((int)a1, (const char *)a2, (int)a3);
    case SYS_RMDIR:      return do_rmdir((const char *)a1);
    case SYS_UNLINK:     return do_unlink((const char *)a1);
    case SYS_UNLINKAT:   return do_unlinkat((int)a1, (const char *)a2, (int)a3);
    case SYS_RENAME:     return do_rename((const char *)a1, (const char *)a2);
    case SYS_RENAMEAT2:  return do_renameat2((int)a1, (const char *)a2,
                                              (int)a3, (const char *)a4, (int)a5);
    case SYS_GETDENTS64: return do_getdents64((int)a1, (void *)a2, (size_t)a3);

    /* Filesystem metadata */
    case SYS_FCHMOD:     return do_fchmod((int)a1, (uint32_t)a2);
    case SYS_FCHOWN:     return do_fchown((int)a1, (uint32_t)a2, (uint32_t)a3);
    case SYS_LINK:       return do_link((const char *)a1, (const char *)a2);
    case SYS_SYMLINK:    return do_symlink((const char *)a1, (const char *)a2);
    case SYS_READLINK:   return do_readlink((const char *)a1, (char *)a2, (size_t)a3);
    case SYS_TRUNCATE:   return do_truncate((const char *)a1, (int64_t)a2);
    case SYS_FTRUNCATE:  return do_ftruncate((int)a1, (int64_t)a2);
    case SYS_FCHMODAT:   return do_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, (int)a4);
    case SYS_UTIMENSAT:  return do_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4);
    case SYS_FALLOCATE:  return do_fallocate((int)a1, (int)a2, (int64_t)a3, (int64_t)a4);
    case SYS_MKNODAT:    return do_mknodat((int)a1, (const char *)a2, (uint32_t)a3, (uint64_t)a4);

    /* Pipe / IO */
    case SYS_PIPE:   return do_pipe2((int *)a1, 0);
    case SYS_PIPE2:  return do_pipe2((int *)a1, (int)a2);
    case SYS_READV:  return do_readv((int)a1, (const struct iovec *)a2, (int)a3);
    case SYS_IOCTL:  return do_ioctl((int)a1, (unsigned long)a2, (unsigned long)a3);
    case SYS_FCNTL:  return do_fcntl((int)a1, (int)a2, a3);

    /* Stubs */
    case SYS_ACCESS: return do_access((const char *)a1);

    /* epoll / eventfd / timerfd / signalfd / inotify */
    case SYS_EPOLL_CREATE1:     return do_epoll_create1((int)a1);
    case SYS_EPOLL_CTL:         return do_epoll_ctl((int)a1, (int)a2, (int)a3,
                                                     (struct epoll_event *)a4);
    case SYS_EPOLL_WAIT:        return do_epoll_wait((int)a1, (struct epoll_event *)a2,
                                                      (int)a3, (int)a4);
    case SYS_EVENTFD2:          return do_eventfd2((unsigned int)a1, (int)a2);
    case SYS_TIMERFD_CREATE:    return do_timerfd_create((int)a1, (int)a2);
    case SYS_TIMERFD_SETTIME:   return do_timerfd_settime((int)a1, (int)a2,
                                         (const struct k_itimerspec *)a3,
                                         (struct k_itimerspec *)a4);
    case SYS_SIGNALFD4:         return do_signalfd4((int)a1, (const uint64_t *)a2, (int)a3);
    case SYS_INOTIFY_INIT1:     return do_inotify_init1((int)a1);
    case SYS_INOTIFY_ADD_WATCH: return do_inotify_add_watch((int)a1, (const char *)a2,
                                                             (uint32_t)a3);
    case SYS_INOTIFY_RM_WATCH:  return do_inotify_rm_watch((int)a1, (int)a2);
    case SYS_EPOLL_PWAIT:       return do_epoll_wait((int)a1, (struct epoll_event *)a2,
                                                      (int)a3, (int)a4); /* ignore sigmask a5 */

    /* ── CosmoRT Hardware Primitives (for userspace drivers) ── */
    /* Capability check: only processes with is_driver may use these */
#define HW_CAP_CHECK() do { \
    process_t *_p = proc_current(); \
    if (!_p || !_p->is_driver) return -EPERM; \
} while (0)

    case SYS_COSMO_MMIO_MAP: {
        HW_CAP_CHECK();
        if (!user_ok(a3, 8)) return -EFAULT;
        void *virt;
        int r = cosmo_mmio_map((uint64_t)a1, (size_t)a2, &virt);
        if (r == 0) kmemcpy((void *)a3, &virt, sizeof(virt));
        return r;
    }
    case SYS_COSMO_DMA_ALLOC: {
        HW_CAP_CHECK();
        if (!user_ok(a2, 8) || !user_ok(a3, 8)) return -EFAULT;
        void *virt; uint64_t phys;
        int r = cosmo_dma_alloc((size_t)a1, &virt, &phys);
        if (r == 0) { kmemcpy((void *)a2, &virt, sizeof(virt)); kmemcpy((void *)a3, &phys, sizeof(phys)); }
        return r;
    }
    case SYS_COSMO_DMA_FREE:
        HW_CAP_CHECK();
        cosmo_dma_free((void *)a1, (size_t)a2);
        return 0;
    case SYS_COSMO_IRQ_REGISTER: {
        HW_CAP_CHECK();
        /* Validate handler address: must be in user-space range */
        if ((uint64_t)a2 >= 0x800000000000ULL || a2 == 0) return -EFAULT;
        return cosmo_irq_register((int)a1, (void (*)(void *))a2, (void *)a3);
    }
    case SYS_COSMO_PCI_READ: {
        HW_CAP_CHECK();
        if (!user_ok(a5, 4)) return -EFAULT;
        return cosmo_pci_config_read((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t *)a5);
    }
    case SYS_COSMO_PCI_WRITE:
        HW_CAP_CHECK();
        return cosmo_pci_config_write((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t)a5);
    case SYS_COSMO_FW_LOAD: {
        HW_CAP_CHECK();
        if (!user_ok(a2, 8) || !user_ok(a3, 8)) return -EFAULT;
        return cosmo_fw_load((const char *)a1, (void **)a2, (size_t *)a3);
    }
    case SYS_COSMO_NIC_ATTACH: {
        HW_CAP_CHECK();
        if (!user_ok(a1, 22)) return -EFAULT;
        struct { uint64_t shm_phys; uint64_t shm_size; uint8_t mac[6]; } kargs;
        kmemcpy(&kargs, (const void *)a1, sizeof(kargs));
        return net_port_attach(kargs.shm_phys, (size_t)kargs.shm_size, kargs.mac);
    }

    case SYS_COSMO_KEXEC: {
        HW_CAP_CHECK();
        if (!user_ok(a1, (size_t)a2)) return -EFAULT;
        extern int do_kexec(const void *, size_t);
        return do_kexec((const void *)a1, (size_t)a2);
    }
#undef HW_CAP_CHECK

    default: {
        process_t *dp = proc_current();
        serial_puts("syscall: unhandled #");
        serial_hex64((uint64_t)num);
        if (dp) { serial_puts(" pid="); serial_putchar('0' + (dp->pid % 10)); }
        serial_putchar('\n');
        return -ENOSYS;
    }
    }
}

long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long result = sys_dispatch(num, a1, a2, a3, a4, a5, a6);
    check_signals_syscall_path(&result, num);
    return result;
}
