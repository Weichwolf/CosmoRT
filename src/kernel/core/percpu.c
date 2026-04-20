/* CosmoRT Per-CPU Data */

#include "core/percpu.h"
#include "hw/serial.h"
#include "core/smp.h"
#include "config.h"
#include "hal/hal.h"

percpu_t percpu_data[SMP_MAX_CORES];

void percpu_init_bsp(void) {
    percpu_t *p = &percpu_data[0];
    p->core_id = 0;
    p->kernel_rsp = 0;
    p->user_rsp = 0;
    p->current_thread = 0;
    p->in_kernel = 1;
    p->self = p;

    /* Prime both KERNEL_GS_BASE (swapgs target) and GS_BASE (active now)
     * so percpu_self() works before the first user->kernel transition. */
    uint64_t base = ensure_high((uint64_t)(uintptr_t)p);
    hal_cpu_set_percpu_base(base);
    hal_cpu_set_percpu_active(base);

    serial_puts("percpu: BSP init\n");
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
