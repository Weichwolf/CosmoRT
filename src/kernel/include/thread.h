/* CosmoRT Thread — schedulable entity
 *
 * Layout: first fields match context.asm proc_enter_ring3 offsets.
 * state@0, tid@4, rsp@8, rip@16, rflags@24, rax@32, ...
 */
#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>

/* Thread states */
#define THREAD_FREE      0
#define THREAD_RUNNABLE  1
#define THREAD_RUNNING   2
#define THREAD_BLOCKED   3
#define THREAD_DEAD      4

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
    /* ── Must match context.asm offsets ── */
    int      state;     /* 0 */
    int      tid;       /* 4 */
    uint64_t rsp;       /* 8 */
    uint64_t rip;       /* 16 */
    uint64_t rflags;    /* 24 */
    uint64_t rax;       /* 32 */
    uint64_t rbx;       /* 40 */
    uint64_t rcx;       /* 48 */
    uint64_t rdx;       /* 56 */
    uint64_t rsi;       /* 64 */
    uint64_t rdi;       /* 72 */
    uint64_t rbp;       /* 80 */
    uint64_t r8;        /* 88 */
    uint64_t r9;        /* 96 */
    uint64_t r10;       /* 104 */
    uint64_t r11;       /* 112 */
    uint64_t r12;       /* 120 */
    uint64_t r13;       /* 128 */
    uint64_t r14;       /* 136 */
    uint64_t r15;       /* 144 */

    /* ── Kernel stack ── */
    uint8_t *kstack;
    uint64_t kstack_top;

    /* ── Scheduling ── */
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

    /* ── CLONE_CHILD_CLEARTID: clear + futex_wake on exit ── */
    int *clear_child_tid;

    /* ── Fault info for signal delivery ── */
    uint64_t fault_addr;   /* CR2 for SIGSEGV/SIGBUS delivery */

    /* ── Timed wakeup (epoll_wait / poll timeout) ── */
    uint64_t wake_at;      /* timer_ms() deadline; 0 = no timeout */
} thread_t;

#define THREAD_MAX 64

/* Thread pool */
extern thread_t thread_pool[];

/* Allocate/free threads from slab pool */
thread_t *thread_alloc(void);
void thread_free(thread_t *t);

#endif
