/* CosmoRT Syscall Dispatcher — routes syscall numbers to handlers */

#include "internal.h"
#include "syscall_table.h"

/* Copy user path string to kernel buffer with full bounds checking.
 * Returns string length (excluding NUL) or negative errno. */
int copy_path_from_user(char *kbuf, const char *upath, size_t max) {
    if (__builtin_expect(!user_ok((uint64_t)upath, 1), 0)) return -EFAULT;
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

    /* Read current FS_BASE from MSR — may differ from t->fs_base
     * if arch_prctl(SET_FS) was called since last context switch */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        t->fs_base = ((uint64_t)hi << 32) | lo;
    }

    /* Save FPU/SSE state so fork/clone get a consistent snapshot */
    __asm__ volatile("fxsave %0" : "=m"(t->fxsave_area));
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
#define HW_CAP_CHECK() do { \
    process_t *_p = proc_current(); \
    if (!_p || !_p->is_driver) return -EPERM; \
} while (0)

    case SYS_COSMO_MMIO_MAP: {
        HW_CAP_CHECK();
        void *virt;
        int r = cosmo_mmio_map((uint64_t)a1, (size_t)a2, &virt);
        if (r == 0) { r = copy_to_user((void *)a3, &virt, sizeof(virt)); if (r) return r; }
        return r;
    }
    case SYS_COSMO_DMA_ALLOC: {
        HW_CAP_CHECK();
        void *virt; uint64_t phys;
        int r = cosmo_dma_alloc((size_t)a1, &virt, &phys);
        if (r == 0) {
            int r2 = copy_to_user((void *)a2, &virt, sizeof(virt)); if (r2) return r2;
            r2 = copy_to_user((void *)a3, &phys, sizeof(phys)); if (r2) return r2;
        }
        return r;
    }
    case SYS_COSMO_DMA_FREE:
        HW_CAP_CHECK();
        cosmo_dma_free((void *)a1, (size_t)a2);
        return 0;
    case SYS_COSMO_IRQ_REGISTER: {
        HW_CAP_CHECK();
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
        struct { uint64_t shm_phys; uint64_t shm_size; uint8_t mac[6]; } kargs;
        { int r = copy_from_user(&kargs, (const void *)a1, sizeof(kargs)); if (r) return r; }
        return net_port_attach(kargs.shm_phys, (size_t)kargs.shm_size, kargs.mac);
    }

    case SYS_COSMO_KEXEC: {
        HW_CAP_CHECK();
        if (!user_ok(a1, (size_t)a2)) return -EFAULT;
        extern int do_kexec(const void *, size_t);
        return do_kexec((const void *)a1, (size_t)a2);
    }
#undef HW_CAP_CHECK

    /* pselect6 / select → convert to poll.
     * pselect6(nfds, readfds, writefds, exceptfds, timeout, sigmask) */
    case 270: /* SYS_PSELECT6 */
    case 23:  /* SYS_SELECT */ {
        int nfds = (int)a1;
        uint64_t *readfds = (uint64_t *)a2;
        /* Minimal: poll the fds in readfds with POLLIN */
        if (nfds <= 0) return 0;
        if (nfds > 64) nfds = 64;
        /* Build pollfd array on kernel stack */
        struct { int fd; short events; short revents; } pfd[64];
        int npfd = 0;
        for (int i = 0; i < nfds && npfd < 64; i++) {
            int in_read = readfds && user_ok((uint64_t)readfds, 8) &&
                          (readfds[i / 64] & (1ULL << (i % 64)));
            if (in_read) {
                pfd[npfd].fd = i;
                pfd[npfd].events = 1; /* POLLIN */
                pfd[npfd].revents = 0;
                npfd++;
            }
        }
        if (npfd == 0) return 0;
        /* Compute timeout in ms */
        int timeout_ms = -1;
        if (num == 270 && a5) { /* pselect6: timespec */
            struct { long sec; long nsec; } ts;
            if (user_ok(a5, 16)) {
                copy_from_user(&ts, (void *)a5, 16);
                timeout_ms = (int)(ts.sec * 1000 + ts.nsec / 1000000);
            }
        } else if (num == 23 && a5) { /* select: timeval */
            struct { long sec; long usec; } tv;
            if (user_ok(a5, 16)) {
                copy_from_user(&tv, (void *)a5, 16);
                timeout_ms = (int)(tv.sec * 1000 + tv.usec / 1000);
            }
        }
        long ret = do_poll(pfd, npfd, timeout_ms);
        /* Write back readfds */
        if (readfds && user_ok((uint64_t)readfds, (uint64_t)((nfds + 63) / 64 * 8))) {
            int nwords = (nfds + 63) / 64;
            uint64_t out[1] = {0};
            for (int i = 0; i < npfd; i++) {
                if (pfd[i].revents & 1)
                    out[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            }
            for (int w = 0; w < nwords && w < 1; w++)
                copy_to_user(&readfds[w], &out[w], 8);
        }
        return ret > 0 ? ret : 0;
    }

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
