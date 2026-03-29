/* CosmoRT Thread — schedulable entity
 *
 * Layout: first fields match context.asm proc_enter_ring3 offsets.
 * state@0, tid@4, rsp@8, rip@16, rflags@24, rax@32, ...
 */
#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include "core/event_queue.h"

/* Thread states */
#define THREAD_FREE      0
#define THREAD_RUNNABLE  1
#define THREAD_RUNNING   2
#define THREAD_BLOCKED   3
#define THREAD_DEAD      4
#define THREAD_STOPPED   5

/* Scheduling policies (POSIX) */
#define SCHED_OTHER  0   /* Default: fair timesharing */
#define SCHED_FIFO   1   /* RT: run until yield/block, no timeslice */
#define SCHED_RR     2   /* RT: timeslice within same priority */

/* Priority range */
#define PRIO_MIN     0   /* SCHED_OTHER runs at priority 0 */
#define PRIO_RT_MIN  1   /* Lowest RT priority */
#define PRIO_RT_MAX  31  /* Highest RT priority */
#define PRIO_LEVELS  32  /* Total priority levels */

/* RR timeslice in scheduler ticks */
#define RR_TIMESLICE 10

struct process; /* forward */

typedef struct thread {
    /* ── Cache-line 0 (bytes 0-63): context-switch hot path ──
     * state, tid, rsp, rip, rflags, rax, rbx, rcx — touched every
     * syscall entry/exit and context switch.
     * WARNING: offsets are hardcoded in context.asm proc_enter_ring3.
     * DO NOT reorder without updating assembly. */
    int      state;     /* 0 */
    int      tid;       /* 4 */
    uint64_t rsp;       /* 8 */
    uint64_t rip;       /* 16 */
    uint64_t rflags;    /* 24 */
    uint64_t rax;       /* 32 */
    uint64_t rbx;       /* 40 */
    uint64_t rcx;       /* 48 */
    uint64_t rdx;       /* 56 */
    /* ── Cache-line 1 (bytes 64-127): remaining GP registers ── */
    uint64_t rsi;       /* 64 */
    uint64_t rdi;       /* 72 */
    uint64_t rbp;       /* 80 */
    uint64_t r8;        /* 88 */
    uint64_t r9;        /* 96 */
    uint64_t r10;       /* 104 */
    uint64_t r11;       /* 112 */
    uint64_t r12;       /* 120 */
    /* ── Cache-line 2 (bytes 128-191): registers + kernel stack ── */
    uint64_t r13;       /* 128 */
    uint64_t r14;       /* 136 */
    uint64_t r15;       /* 144 */

    /* ── Kernel stack ── */
    uint8_t *kstack;
    uint64_t kstack_top;

    /* ── Scheduling (cache-line 3+): per-tick checks ── */
    int      sched_policy;
    int      priority;           /* 0 = normal, 1-31 = RT */
    int      saved_priority;     /* original priority before PI boost, -1 = not boosted */
    uint64_t fs_base;            /* per-thread FS base for TLS */
    int      cpu_affinity;       /* -1 = any core, 0..SMP_MAX_CORES-1 = pinned */
    uint64_t timeslice;          /* remaining ticks for SCHED_RR */

    /* ── Owner ── */
    struct process *proc;

    /* ── Linkage ── */
    struct thread *rq_next;      /* scheduler run queue */
    struct thread *proc_next;    /* process thread list */

    /* ── Kernel context (setjmp buffer) ── */
    uint64_t jmpbuf[8];

    /* ── Kernel-level yield: resume in-kernel call chain (e.g., net_idle) ── */
    uint64_t kernel_yield_jmpbuf[8];
    int      in_kernel_yield;   /* 1 = resume via kernel_yield_jmpbuf */

    /* ── CLONE_CHILD_CLEARTID: clear + futex_wake on exit ── */
    int *clear_child_tid;

    /* ── Robust Mutex List: kernel walks on thread exit ── */
    void *robust_list;     /* userspace robust_list_head pointer */

    /* ── Fault info for signal delivery ── */
    uint64_t fault_addr;   /* CR2 for SIGSEGV/SIGBUS delivery */

    /* ── Timed wakeup (epoll_wait / poll timeout) ── */
    uint64_t wake_at;      /* timer_ms() deadline; 0 = no timeout */

    /* ── nanosleep: absolute deadline for syscall-restart detection ── */
    uint64_t nanosleep_deadline;  /* timer_ms() deadline; 0 = not in nanosleep */

    /* ── Signal mask + per-thread pending (like Linux task_struct) ── */
    uint64_t sig_blocked;     /* bitmask of blocked signals (sigprocmask) */
    uint64_t sig_thread_pending; /* per-thread pending (from tgkill/tkill) */

    /* ── rt_sigsuspend saved mask (restored before signal delivery) ── */
    uint64_t sig_saved_mask;  /* old sig_blocked during sigsuspend; 0 = not in sigsuspend */
    int      in_sigsuspend;   /* 1 while blocked in rt_sigsuspend */

    /* ── sigaltstack ── */
    uint64_t sigalt_sp;       /* ss_sp: base of alternate signal stack */
    uint64_t sigalt_size;     /* ss_size: size of alternate signal stack */
    int      sigalt_flags;    /* ss_flags: SS_DISABLE etc. */

    /* ── Event Queue (per-thread, for blocking syscalls) ── */
    event_queue_t eq;

    /* ── FPU/SSE state (FXSAVE/FXRSTOR) ── */
    uint8_t fxsave_area[512] __attribute__((aligned(16)));

    /* ── TSC-based wakeup deadline (after fxsave to avoid offset shifts) ── */
    uint64_t wake_at_tsc;  /* TSC deadline for sub-ms precision; 0 = use wake_at */
} thread_t;

/* Thread pool — dynamically allocated via slab */

/* Allocate/free threads from slab pool */
thread_t *thread_alloc(void);
void thread_free(thread_t *t);

#endif
