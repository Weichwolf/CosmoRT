/* CosmoRT Syscall Dispatcher — routes syscall numbers to handlers */

#include "internal.h"
#include "syscall_table.h"

/* Copy user path string to kernel buffer with full bounds checking.
 * Returns string length (excluding NUL) or negative errno. */
int copy_path_from_user(char *kbuf, const char *upath, size_t max) {
    if (__builtin_expect(!user_ok((uint64_t)upath, max), 0)) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
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

    /* Read current FS_BASE from MSR — may differ from t->fs_base
     * if arch_prctl(SET_FS) was called since last context switch */
    t->fs_base = arch_get_fs_base();

    /* Save FPU/SSE state so fork/clone get a consistent snapshot */
    arch_fxsave(t->fxsave_area);
}

/* ── Cold-path error helpers (keep strings out of hot dispatch) ── */

__attribute__((cold))
static void dispatch_unhandled(long num, int pid) {
    serial_puts("syscall: unhandled #");
    serial_hex64((uint64_t)num);
    if (pid >= 0) { serial_puts(" pid="); serial_putchar('0' + (pid % 10)); }
    serial_putchar('\n');
}

/* ── Dispatcher ──────────────────────────────────── */

__attribute__((hot))
static long sys_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    switch (num) {

    /* ── X-Macro generated cases ── */
#define X_DISPATCH(nr, name, nargs, handler) case nr: return handler;
    SYSCALL_TABLE(X_DISPATCH)
#undef X_DISPATCH

    /* ── Cases with inline logic (not in table) ── */

    /* exit: no return value */
    case SYS_EXIT:          do_exit((int)a1); return 0;
    case SYS_EXIT_GROUP:    do_exit_group((int)a1); return 0;

    /* set_tid_address: touches thread struct */
    case SYS_SET_TID_ADDRESS: {
        thread_t *t = thread_current();
        if (t && user_ok((uint64_t)a1, 4))
            t->clear_child_tid = (int *)a1;
        return t ? (long)t->tid : 1;
    }

    /* Identity: need proc/thread struct */
    case SYS_GETPID:  { process_t *p = proc_current(); return p ? (long)p->pid : 1; }
    case SYS_GETPPID: { process_t *p = proc_current(); return p ? (long)p->parent_pid : 0; }
    case SYS_GETTID:  { thread_t *t = thread_current(); return t ? (long)t->tid : 1; }

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

    /* dup: find lowest free fd */
    case SYS_DUP: {
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
                } else if (dold->obj) {
                    fd_obj_incref(dold->type, dold->obj);
                }
                if (di >= dp->fds.max_fd) dp->fds.max_fd = di + 1;
                return di;
            }
        }
        return -EMFILE;
    }

    /* dup2: special case oldfd==newfd */
    case SYS_DUP2: {
        if ((int)a1 == (int)a2) {
            process_t *p = proc_current();
            if (!p) return -EFAULT;
            return fd_get(&p->fds, (int)a1) ? (int)a2 : -EBADF;
        }
        return do_dup3((int)a1, (int)a2, 0);
    }

    /* accept4: dispatch to unix socket or TCP */
    case SYS_ACCEPT4: {
        process_t *a4p = proc_current();
        if (a4p) {
            fd_entry_t *a4e = fd_get(&a4p->fds, (int)a1);
            if (a4e && a4e->type == FD_UNIX_SOCK)
                return usock_accept4((int)a1, (void *)a2, (int *)a3, (int)a4);
        }
        return do_accept((int)a1, (void *)a2, (int *)a3);
    }

    /* socketpair: AF_UNIX only */
    case SYS_SOCKETPAIR: {
        if ((int)a1 != 1 /* AF_UNIX */) return -EAFNOSUPPORT;
        return usock_socketpair((int)a2, (int *)a4);
    }

    /* sendmmsg(307): dispatch to sys_net.c */
    case 307:
        return do_sendmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4);

    /* futex: user_ok check before dispatch */
    case SYS_FUTEX:
        if (__builtin_expect(!user_ok((uint64_t)a1, 4), 0)) return -EFAULT;
        return do_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                                        (const struct timespec *)a4,
                                        (uint32_t *)a5, (uint32_t)a6);

    /* time(2): inline epoch calculation */
    case SYS_TIME: {
        extern uint64_t timer_ms(void);
        extern uint64_t rtc_epoch_sec;
        long secs = (long)(timer_ms() / 1000 + rtc_epoch_sec);
        if (a1) copy_to_user((void *)a1, &secs, sizeof(secs));
        return secs;
    }

    /* xattr: return -ENODATA ("no attributes") instead of -ENOSYS */
    case SYS_SETXATTR:  case SYS_LSETXATTR:  case SYS_FSETXATTR:
    case SYS_GETXATTR:  case SYS_LGETXATTR:  case SYS_FGETXATTR:
    case SYS_LISTXATTR: case SYS_LLISTXATTR: case SYS_FLISTXATTR:
    case SYS_REMOVEXATTR: case SYS_LREMOVEXATTR: case SYS_FREMOVEXATTR:
        return -ENODATA;

    /* ── CosmoRT Hardware Primitives (for userspace drivers) ── */
    case SYS_COSMO_MMIO_MAP:    return do_cosmo_mmio_map(a1, a2, a3);
    case SYS_COSMO_DMA_ALLOC:   return do_cosmo_dma_alloc(a1, a2, a3);
    case SYS_COSMO_DMA_FREE:    return do_cosmo_dma_free(a1, a2);
    case SYS_COSMO_IRQ_REGISTER:return do_cosmo_irq_register(a1, a2, a3);
    case SYS_COSMO_PCI_READ:    return do_cosmo_pci_read(a1, a2, a3, a4, a5);
    case SYS_COSMO_PCI_WRITE:   return do_cosmo_pci_write(a1, a2, a3, a4, a5);
    case SYS_COSMO_FW_LOAD:     return do_cosmo_fw_load(a1, a2, a3);
    case SYS_COSMO_NIC_ATTACH:  return do_cosmo_nic_attach(a1);
    case SYS_COSMO_KEXEC:       return do_cosmo_kexec(a1, a2);
    case SYS_COSMO_RT_QUERY: {
        extern int rt_core_id(int);
        extern int rt_is_current_rt(void);
        extern void rt_wake(int core_id);
        /* a1=0: rt_is_current_rt(), a1=1: rt_core_id(a2), a1=2: rt_wake(a2) */
        if (a1 == 0) return (long)rt_is_current_rt();
        if (a1 == 1) return (long)rt_core_id((int)a2);
        if (a1 == 2) { rt_wake((int)a2); return 0; }
        return -EINVAL;
    }

    /* pselect6 / select → sys_event.c */
    case 270: /* SYS_PSELECT6 */
    case 23:  /* SYS_SELECT */
        return do_pselect6((int)a1, (uint64_t *)a2, a3, a4, a5, num);

    /* ppoll(271) → sys_event.c */
    case 271:
        return do_ppoll(a1, a2, a3);

    /* preadv(295) / pwritev(296): readv/writev + file offset */
    case 295: { /* preadv(fd, iov, iovcnt, offset_lo, offset_hi) */
        /* For now: ignore offset, delegate to readv (files have internal offset) */
        return do_readv((int)a1, (const struct iovec *)a2, (int)a3);
    }
    case 296: { /* pwritev */
        return do_writev((int)a1, (const struct iovec *)a2, (int)a3);
    }

    /* recvmmsg(299) → sys_net.c */
    case 299:
        return do_recvmmsg((int)a1, (uint64_t)a2, (int)a3, (int)a4);

    default: {
        process_t *dp = proc_current();
        dispatch_unhandled(num, dp ? (int)dp->pid : -1);
        return -ENOSYS;
    }
    }
}

__attribute__((hot))
long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    /* Arm fault recovery: if the kernel page-faults on an unmapped user
     * address during syscall execution, longjmp returns here with val=1
     * and we return -EFAULT instead of panicking. */
    extern int kernel_setjmp(uint64_t buf[8]);
    percpu_t *cpu = percpu_self();
    if (kernel_setjmp(cpu->fault_jmpbuf) != 0) {
        /* Returned from fault recovery — user pointer was bad */
        cpu->fault_recover = 0;
        return -EFAULT;
    }
    cpu->fault_recover = 1;
    long result = sys_dispatch(num, a1, a2, a3, a4, a5, a6);
    cpu->fault_recover = 0;
    check_signals_syscall_path(&result, num);
    return result;
}
