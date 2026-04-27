/* CosmoRT Syscall Dispatcher — routes syscall numbers to handlers */

#include "internal.h"
#include "syscall_table.h"

/* copy_path_from_user lives in src/arch/x86_64/cpu/uaccess.c - it wraps
 * a single user-touching instruction with _ASM_EXTABLE for fault recovery. */

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

    /* Read current FS/GS_BASE from MSR — may differ from t->fs_base/gs_base
     * if arch_prctl(SET_FS/SET_GS) was called since last context switch.
     * (User GS lives in IA32_KERNEL_GS_BASE while CPU is in kernel mode.) */
    t->fs_base = hal_cpu_get_tls();
    t->gs_base = hal_cpu_get_user_gs();

    /* Save FPU/SSE/AVX state so fork/clone get a consistent snapshot */
    hal_cpu_fpu_save(t->xsave_area);
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

/* CosmoRT-eigene Syscalls (0x10000+) — eigener Dispatch, kein Switch-Pollution */
extern long cosmo_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6);

__attribute__((hot))
static long sys_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    /* CosmoRT syscalls: separate dispatch for 0x10000+ range */
    if (__builtin_expect(num >= 0x10000, 0))
        return cosmo_dispatch(num, a1, a2, a3, a4, a5, a6);

    switch (num) {

    /* ── X-Macro generated cases ── */
#define X_DISPATCH(nr, name, handler) case nr: return handler;
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

    /* dup: find lowest free fd. POSIX: new fd does NOT inherit O_CLOEXEC. */
    case SYS_DUP: {
        process_t *dp = proc_current();
        if (!dp) return -EFAULT;
        fd_entry_t *dold = fd_get(&dp->fds, (int)a1);
        if (!dold) return -EBADF;
        fd_entry_t src = *dold;
        int di = fd_dup_at(&dp->fds, 0, src, src.flags & ~O_CLOEXEC);
        if (di < 0) return di;
        if (src.type == FD_FILE && src.obj) {
            extern void vfs_file_incref(struct vfs_file *f);
            vfs_file_incref((struct vfs_file *)src.obj);
        } else if (src.obj) {
            fd_obj_incref(src.type, src.obj, src.flags);
        }
        return di;
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

    /* accept4: now handled via syscall_table (do_accept4 with flags) */

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
        extern int64_t rtc_epoch_sec;
        long secs = (long)((int64_t)(timer_ms() / 1000) + rtc_epoch_sec);
        if (a1) copy_to_user((void *)a1, &secs, sizeof(secs));
        return secs;
    }

    /* alarm(2): per-process SIGALRM timer */
    case SYS_ALARM: return do_alarm((unsigned int)a1);

    /* xattr: no FS backend supports them. Linux-konform:
     *   fd-based syscalls prüfen zuerst fd -> -EBADF
     *   path-based syscalls prüfen zuerst Pfad -> Lookup-Errno
     *   danach: -ENOTSUP (kein xattr-Support). Für listxattr mit size==0
     *   ist Linux-Verhalten: return 0 (benötigte Buffer-Größe = 0). */
    case SYS_FSETXATTR: case SYS_FGETXATTR: case SYS_FREMOVEXATTR:
    case SYS_FLISTXATTR: {
        process_t *xp = proc_current();
        if (!xp) return -EFAULT;
        fd_entry_t *fde = fd_get(&xp->fds, (int)a1);
        if (!fde || fde->type == FD_NONE) return -EBADF;
        if (num == SYS_FLISTXATTR && a3 == 0) return 0;
        if (num == SYS_FGETXATTR) return -ENODATA;
        return -ENOTSUP;
    }
    case SYS_SETXATTR:  case SYS_LSETXATTR:
    case SYS_GETXATTR:  case SYS_LGETXATTR:
    case SYS_LISTXATTR: case SYS_LLISTXATTR:
    case SYS_REMOVEXATTR: case SYS_LREMOVEXATTR: {
        char xpath[PATH_MAX];
        int xlen = copy_path_from_user(xpath, (const char *)a1, PATH_MAX);
        if (xlen < 0) return xlen;
        int xerr = 0;
        extern struct vfs_node *vfs_lookup_err(const char *path, int *err);
        struct vfs_node *xn = vfs_lookup_err(xpath, &xerr);
        if (!xn) return xerr;
        if ((num == SYS_LISTXATTR || num == SYS_LLISTXATTR) && a3 == 0) return 0;
        if (num == SYS_GETXATTR || num == SYS_LGETXATTR) return -ENODATA;
        return -ENOTSUP;
    }

    default: {
        process_t *dp = proc_current();
        dispatch_unhandled(num, dp ? (int)dp->pid : -1);
        return -ENOSYS;
    }
    }
}

__attribute__((hot))
long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long result = sys_dispatch(num, a1, a2, a3, a4, a5, a6);
    check_signals_syscall_path(&result, num);
    return result;
}
