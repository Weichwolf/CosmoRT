/* HAL — CPU primitives (arch-agnostic)
 *
 * Implemented per-architecture in arch/{x86_64,aarch64}/cpu/.
 * No kernel code outside arch/ should use raw instructions directly.
 */
#ifndef HAL_CPU_H
#define HAL_CPU_H

#include <stdint.h>

/* CPU identification */
int  hal_cpu_id(void);                    /* current core ID (APIC ID / MPIDR) */
int  hal_cpu_count(void);                 /* number of online cores */

/* CPU control */
void hal_cpu_halt(void);                  /* HLT / WFI — wait for interrupt */
void hal_cpu_relax(void);                 /* PAUSE / YIELD — spin-loop hint */
void hal_cpu_cli(void);                   /* disable interrupts */
void hal_cpu_sti(void);                   /* enable interrupts */

/* Interrupt state */
uint64_t hal_cpu_save_irq(void);          /* save RFLAGS/DAIF, disable IRQs */
void     hal_cpu_restore_irq(uint64_t);   /* restore saved IRQ state */

/* Memory barriers */
void hal_cpu_mfence(void);                /* full memory fence */
void hal_cpu_wmb(void);                   /* write barrier */
void hal_cpu_rmb(void);                   /* read barrier */

/* Monotonic timestamp (TSC / CNTVCT) */
uint64_t hal_cpu_timestamp(void);         /* raw monotonic counter */

/* TLS base (FS_BASE / TPIDR_EL0) */
void     hal_cpu_set_tls(uint64_t base);
uint64_t hal_cpu_get_tls(void);

/* FPU/SIMD state */
void hal_cpu_fpu_save(void *area);
void hal_cpu_fpu_restore(const void *area);

/* Kernel GS / percpu base */
void hal_cpu_set_percpu_base(uint64_t base);

#endif
