/* CosmoRT Thread — schedulable entity */
#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include "core/event_queue.h"

#define THREAD_FREE      0
#define THREAD_RUNNABLE  1
#define THREAD_RUNNING   2
#define THREAD_BLOCKED   3
#define THREAD_DEAD      4
#define THREAD_STOPPED   5

#define SCHED_OTHER  0
#define SCHED_FIFO   1
#define SCHED_RR     2

#define PRIO_MIN     0
#define PRIO_RT_MIN  1
#define PRIO_RT_MAX  31
#define PRIO_LEVELS  32

#define RR_TIMESLICE 10

struct process;

typedef struct thread {
    int      state;
    int      tid;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint8_t *kstack;
    uint64_t kstack_top;

    int      sched_policy;
    int      priority;
    int      saved_priority;
    int      preempt_count;
    volatile int need_resched;
    uint64_t fs_base;
    int      cpu_affinity;
    uint64_t timeslice;

    struct process *proc;

    struct thread *rq_next;
    struct thread *proc_next;

    uint64_t jmpbuf[8];

    uint64_t kernel_yield_jmpbuf[8];
    int      in_kernel_yield;

    int *clear_child_tid;

    uint64_t fault_addr;

    uint64_t wake_at;

    uint64_t nanosleep_deadline;

    uint64_t sig_blocked;
    uint64_t sig_thread_pending;

    uint64_t sig_saved_mask;
    int      in_sigsuspend;

    uint64_t sigalt_sp;
    uint64_t sigalt_size;
    int      sigalt_flags;

    event_queue_t eq;

    uint8_t fxsave_area[512] __attribute__((aligned(16)));

    uint64_t wake_at_tsc;

    void *robust_list;

    void (*kthread_fn)(void *);
    void *kthread_arg;
} thread_t;

thread_t *thread_alloc(void);
void thread_free(thread_t *t);

#endif
