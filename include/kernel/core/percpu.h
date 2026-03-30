/* CosmoRT Per-CPU Data — accessed via swapgs + GS-relative addressing */
#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>
#include "config.h"

struct thread;

typedef struct percpu {
    uint64_t       kernel_rsp;
    uint64_t       user_rsp;
    struct thread *current_thread;
    int            core_id;
    int            in_kernel;
    uint64_t       syscall_frame;
    struct percpu *self;
    uint64_t       fault_jmpbuf[8];
    int            fault_recover;
    volatile int   need_resched;
    int            in_preempt;
} percpu_t;

extern percpu_t percpu_data[SMP_MAX_CORES];

void percpu_init_bsp(void);

void percpu_init_ap(int core_id);

static inline percpu_t *percpu_self(void) {
    percpu_t *p;
    __asm__ volatile("mov %%gs:40, %0" : "=r"(p));
    if (__builtin_expect(p != 0, 1)) return p;
    extern percpu_t *percpu_self_slow(void);
    return percpu_self_slow();
}

struct thread *thread_current(void);

#endif
