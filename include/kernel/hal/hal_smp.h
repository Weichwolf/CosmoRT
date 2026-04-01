/* HAL — SMP (arch-agnostic)
 *
 * Abstracts AP bringup: SIPI (x86) / PSCI (ARM).
 */
#ifndef HAL_SMP_H
#define HAL_SMP_H

#include <stdint.h>

/* Boot secondary CPU. entry = kernel function to call on AP. */
int  hal_smp_boot_ap(int cpu_id, void (*entry)(void), void *stack);

/* Number of available (hardware) CPUs */
int  hal_smp_num_cpus(void);

/* Per-CPU data access */
void *hal_smp_percpu(int cpu_id);

#endif
