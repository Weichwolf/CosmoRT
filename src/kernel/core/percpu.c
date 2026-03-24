/* CosmoRT Per-CPU Data */

#include "percpu.h"
#include "serial.h"
#include "smp.h"
#include "config.h"
#include "arch_x86.h"

percpu_t percpu_data[SMP_MAX_CORES];

static inline void wrmsr(uint32_t msr, uint64_t val) { arch_wrmsr(msr, val); }

/* IA32_KERNEL_GS_BASE (MSR 0xC0000102): swapped with GS_BASE on swapgs */
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_GS_BASE        0xC0000101

void percpu_init_bsp(void) {
    percpu_t *p = &percpu_data[0];
    p->core_id = 0;
    p->kernel_rsp = 0;
    p->user_rsp = 0;
    p->current_thread = 0;
    p->in_kernel = 1;
    p->self = p;

    /* Set KERNEL_GS_BASE so swapgs in SYSCALL entry loads our percpu */
    wrmsr(MSR_KERNEL_GS_BASE, ensure_high((uint64_t)(uintptr_t)p));
    /* Also set GS_BASE so percpu_self works before first swapgs */
    wrmsr(MSR_GS_BASE, ensure_high((uint64_t)(uintptr_t)p));

    serial_puts("percpu: BSP init\n");
}

void percpu_init_ap(int core_id) {
    if (core_id < 0 || core_id >= SMP_MAX_CORES) return;
    percpu_t *p = &percpu_data[core_id];
    p->core_id = core_id;
    p->kernel_rsp = 0;
    p->user_rsp = 0;
    p->current_thread = 0;
    p->in_kernel = 1;
    p->self = p;

    wrmsr(MSR_KERNEL_GS_BASE, ensure_high((uint64_t)(uintptr_t)p));
    wrmsr(MSR_GS_BASE, ensure_high((uint64_t)(uintptr_t)p));
}

/* Slow path: LAPIC MMIO lookup (early boot before GS is set) */
percpu_t *percpu_self_slow(void) {
    int id = smp_core_id();
    if (id < 0 || id >= SMP_MAX_CORES) id = 0;
    return &percpu_data[id];
}

struct thread *thread_current(void) {
    return percpu_self()->current_thread;
}
