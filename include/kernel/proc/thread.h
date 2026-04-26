/* CosmoRT Thread — schedulable entity
 *
 * Layout: first fields match context.asm proc_enter_ring3 offsets.
 * state@0, tid@4, rsp@8, rip@16, rflags@24, rax@32, ...
 */
#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include "config.h"
#include "core/event_queue.h"
#include "spinlock.h"

/* Thread states */
#define THREAD_FREE      0
#define THREAD_RUNNABLE  1
#define THREAD_RUNNING   2
#define THREAD_BLOCKED   3
#define THREAD_DEAD      4
#define THREAD_STOPPED   5

/* TASK_* — bit aliases of THREAD_* for try_to_wake_up's mask argument.
 * "Wecke nur, wenn t->state in mask." TASK_INTERRUPTIBLE und
 * TASK_UNINTERRUPTIBLE kollabieren bei uns auf denselben Speicherwert
 * (THREAD_BLOCKED) — die Trennung sitzt im Sleep-Loop, nicht im State. */
#define TASK_RUNNING         (1u << THREAD_RUNNING)
#define TASK_INTERRUPTIBLE   (1u << THREAD_BLOCKED)
#define TASK_UNINTERRUPTIBLE (1u << THREAD_BLOCKED)
#define TASK_STOPPED         (1u << THREAD_STOPPED)
#define TASK_KILLABLE        TASK_INTERRUPTIBLE
#define TASK_NORMAL          (TASK_INTERRUPTIBLE | TASK_STOPPED)

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
struct restart_block; /* forward */

/* Restart-block: per-thread Resume-Closure for syscalls returning
 * -ERESTART_RESTARTBLOCK (clock_nanosleep, futex_wait, poll/select).
 * Linux x86_64 ABI: SYS_restart_syscall (219) calls fn(rb), which
 * resumes the original op with saved deadline / state.
 *
 * fn == NULL means no pending restart -> SYS_restart_syscall returns -EINTR. */
typedef long (*restart_fn_t)(struct restart_block *);

struct restart_block {
    restart_fn_t fn;
    union {
        /* clock_nanosleep / nanosleep restart.
         *
         * mode == 0: relative-style (do_nanosleep). expires_ms is the
         *            kernel CLOCK_MONOTONIC timer_ms() deadline; on resume,
         *            sleep until timer_ms() >= expires_ms. user_rmtp
         *            is updated on EINTR resume.
         * mode == 1: ABSTIME-style. expires_ms is the kernel deadline in
         *            timer_ms() units (already adjusted for time_ns/epoch
         *            at original-call time). user_rmtp == NULL. */
        struct {
            int      clockid;          /* original clk_id (for diagnostics) */
            int      mode;             /* 0=relative, 1=ABSTIME */
            uint64_t expires_ms;       /* kernel timer_ms() deadline */
            void    *user_rmtp;        /* user struct k_timespec * (or NULL) */
        } nanosleep;
        /* futex_wait restart */
        struct {
            uint32_t *uaddr;
            uint32_t  val;
            uint32_t  flags;           /* FUTEX_PRIVATE_FLAG etc */
            uint32_t  bitset;
            uint64_t  deadline_ms;     /* timer_ms() deadline, 0 = infinite */
        } futex;
    };
};

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

    /* ── Kernel context (context_switch RSP) ── */
    uint64_t kstack_rsp;

    /* ── Per-thread percpu state (saved/restored by schedule()) ── */
    uint64_t saved_user_rsp;     /* percpu->user_rsp: sysret needs it per-thread */

    /* ── CLONE_CHILD_CLEARTID: clear + futex_wake on exit ── */
    int *clear_child_tid;

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

    /* ── FPU/SSE/AVX state (XSAVE/XRSTOR, FXSAVE fallback) ── */
    uint8_t xsave_area[XSAVE_MAX_SIZE] __attribute__((aligned(64)));

    /* ── Robust Mutex List (at end to avoid shifting fxsave alignment) ── */
    void *robust_list;     /* userspace robust_list_head pointer */

    /* ── RCU (Preemptible) — read-side critical section tracking ── */
    int             rcu_read_nesting;  /* >0 = inside rcu_read_lock section */
    void           *rcu_blocked_node;  /* rcu_node_t* if preempted during CS, else NULL */
    struct thread  *rcu_next;          /* blocked reader list: forward link */
    struct thread  *rcu_prev;          /* blocked reader list: back link (O(1) removal) */

    /* Serializes eq producers (IRQ + syscall) and ring growth. */
    spinlock_t      eq_lock;

    /* ── Restart block (Linux current->restart_block) ── */
    struct restart_block restart_block;
} thread_t;

/* Thread pool — dynamically allocated via slab */

/* Allocate/free threads from slab pool */
thread_t *thread_alloc(void);
void thread_free(thread_t *t);

#endif
