/* CosmoRT Syscall Dispatcher — routes syscall numbers to handlers */

#include "internal.h"
#include "syscall_table.h"

int copy_path_from_user(char *kbuf, const char *upath, size_t max) {
    if (__builtin_expect(!user_ok((uint64_t)upath, max), 0)) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
        kbuf[i] = upath[i];
        if (kbuf[i] == '\0') return (int)i;
    }
    return -ENAMETOOLONG;
}

void save_user_state_for_block(thread_t *t, long return_value) {
    percpu_t *cpu = percpu_self();
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    t->rip    = frame->rcx;
    t->rflags = frame->r11;
    t->rsp    = cpu->user_rsp;
    t->rax    = (uint64_t)return_value;
    t->rbx = frame->rbx; t->rcx = frame->rcx; t->rdx = frame->rdx;
    t->rsi = frame->rsi; t->rdi = frame->rdi; t->rbp = frame->rbp;
    t->r8  = frame->r8;  t->r9  = frame->r9;  t->r10 = frame->r10;
    t->r11 = frame->r11; t->r12 = frame->r12; t->r13 = frame->r13;
    t->r14 = frame->r14; t->r15 = frame->r15;

    t->fs_base = arch_get_fs_base();

    arch_fxsave(t->fxsave_area);
}

__attribute__((cold))
static void dispatch_unhandled(long num, int pid) {
    serial_puts("syscall: unhandled #");
    serial_hex64((uint64_t)num);
    if (pid >= 0) { serial_puts(" pid="); serial_putchar('0' + (pid % 10)); }
    serial_putchar('\n');
}

extern long cosmo_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6);

__attribute__((hot))
static long sys_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    if (__builtin_expect(num >= 0x10000, 0))
        return cosmo_dispatch(num, a1, a2, a3, a4, a5, a6);

    switch (num) {

#define X_DISPATCH(nr, name, handler) case nr: return handler;
    SYSCALL_TABLE(X_DISPATCH)
#undef X_DISPATCH

    case SYS_EXIT:          do_exit((int)a1); return 0;
    case SYS_EXIT_GROUP:    do_exit_group((int)a1); return 0;

    case SYS_SET_TID_ADDRESS: {
        thread_t *t = thread_current();
        if (t && user_ok((uint64_t)a1, 4))
            t->clear_child_tid = (int *)a1;
        return t ? (long)t->tid : 1;
    }

    case SYS_GETPID:  { process_t *p = proc_current(); return p ? (long)p->pid : 1; }
    case SYS_GETPPID: { process_t *p = proc_current(); return p ? (long)p->parent_pid : 0; }
    case SYS_GETTID:  { thread_t *t = thread_current(); return t ? (long)t->tid : 1; }

    case SYS_SETPGID: {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        int target_pid = (int)a1;
        int new_pgid   = (int)a2;
        process_t *target = (target_pid == 0) ? p : proc_find((uint32_t)target_pid);
        if (!target || target->state != PROC_ALIVE) return -ESRCH;
        if (target != p && target->parent_pid != p->pid) return -ESRCH;
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
        if (p->pgid == p->pid) {
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

    case SYS_DUP: {
        process_t *dp = proc_current();
        if (!dp) return -EFAULT;
        fd_entry_t *dold = fd_get(&dp->fds, (int)a1);
        if (!dold) return -EBADF;
        int di = fd_find_free(&dp->fds, 0);
        if (di < 0) return -EMFILE;
        dp->fds.entries[di] = *dold;
        dp->fds.entries[di].flags &= ~O_CLOEXEC;
        fd_mark_used(&dp->fds, di);
        if (dold->type == FD_FILE && dold->obj) {
            extern void vfs_file_incref(struct vfs_file *f);
            vfs_file_incref((struct vfs_file *)dold->obj);
        } else if (dold->obj) {
            fd_obj_incref(dold->type, dold->obj);
        }
        if (di >= dp->fds.max_fd) dp->fds.max_fd = di + 1;
        return di;
    }

    case SYS_DUP2: {
        if ((int)a1 == (int)a2) {
            process_t *p = proc_current();
            if (!p) return -EFAULT;
            return fd_get(&p->fds, (int)a1) ? (int)a2 : -EBADF;
        }
        return do_dup3((int)a1, (int)a2, 0);
    }

    case SYS_ACCEPT4: {
        process_t *a4p = proc_current();
        if (a4p) {
            fd_entry_t *a4e = fd_get(&a4p->fds, (int)a1);
            if (a4e && a4e->type == FD_UNIX_SOCK)
                return usock_accept4((int)a1, (void *)a2, (int *)a3, (int)a4);
        }
        return do_accept((int)a1, (void *)a2, (int *)a3);
    }

    case SYS_SOCKETPAIR: {
        if ((int)a1 != 1) return -EAFNOSUPPORT;
        return usock_socketpair((int)a2, (int *)a4);
    }

    case SYS_FUTEX:
        if (__builtin_expect(!user_ok((uint64_t)a1, 4), 0)) return -EFAULT;
        return do_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                                        (const struct timespec *)a4,
                                        (uint32_t *)a5, (uint32_t)a6);

    case SYS_TIME: {
        extern uint64_t timer_ms(void);
        extern uint64_t rtc_epoch_sec;
        long secs = (long)(timer_ms() / 1000 + rtc_epoch_sec);
        if (a1) copy_to_user((void *)a1, &secs, sizeof(secs));
        return secs;
    }

    case SYS_ALARM: return do_alarm((unsigned int)a1);

    case SYS_SETXATTR:  case SYS_LSETXATTR:  case SYS_FSETXATTR:
    case SYS_GETXATTR:  case SYS_LGETXATTR:  case SYS_FGETXATTR:
    case SYS_LISTXATTR: case SYS_LLISTXATTR: case SYS_FLISTXATTR:
    case SYS_REMOVEXATTR: case SYS_LREMOVEXATTR: case SYS_FREMOVEXATTR:
        return -ENODATA;

    default: {
        process_t *dp = proc_current();
        dispatch_unhandled(num, dp ? (int)dp->pid : -1);
        return -ENOSYS;
    }
    }
}

__attribute__((hot))
long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    extern int kernel_setjmp(uint64_t buf[8]);
    percpu_t *cpu = percpu_self();
    if (kernel_setjmp(cpu->fault_jmpbuf) != 0) {
        cpu->fault_recover = 0;
        return -EFAULT;
    }
    cpu->fault_recover = 1;
    long result = sys_dispatch(num, a1, a2, a3, a4, a5, a6);
    cpu->fault_recover = 0;
    check_signals_syscall_path(&result, num);
    return result;
}
