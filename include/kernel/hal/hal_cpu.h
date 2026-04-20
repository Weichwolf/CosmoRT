/* HAL - CPU primitives (arch-agnostic)
 *
 * Implemented per-architecture in arch/{x86_64,aarch64}/cpu/.
 * Kernel code above arch/ uses hal_cpu_* exclusively - never raw
 * instructions or arch_* intrinsics. aarch64 port drops in by
 * providing a matching implementation.
 */
#ifndef HAL_CPU_H
#define HAL_CPU_H

#include <stdint.h>
#include <stddef.h>

/* CPU identification */
int  hal_cpu_id(void);                    /* current core ID (APIC ID / MPIDR) */
int  hal_cpu_count(void);                 /* number of online cores */

/* CPU control */
void hal_cpu_halt(void);                  /* enable IRQs, halt until IRQ (STI+HLT / WFI) */
void hal_cpu_halt_noirq(void);            /* halt with IRQs disabled (panic path) */
void hal_cpu_relax(void);                 /* PAUSE / YIELD - spin-loop hint */
void hal_cpu_cli(void);                   /* disable interrupts */
void hal_cpu_sti(void);                   /* enable interrupts */

/* Interrupt state */
uint64_t hal_cpu_save_irq(void);          /* save RFLAGS/DAIF, disable IRQs */
void     hal_cpu_restore_irq(uint64_t);   /* restore saved IRQ state */

/* Memory barriers */
void hal_cpu_mfence(void);                /* full memory fence */
void hal_cpu_wmb(void);                   /* write barrier */
void hal_cpu_rmb(void);                   /* read barrier */

/* Acquire/Release ordering for lockfree queues */
void     hal_cpu_store_release(volatile uint32_t *p, uint32_t v);
uint32_t hal_cpu_load_acquire(volatile uint32_t *p);

/* Monotonic timestamp (TSC / CNTVCT) - raw counter, not nanoseconds */
uint64_t hal_cpu_timestamp(void);

/* Stack pointer snapshot (entropy source, debugging) */
uint64_t hal_cpu_stack_ptr(void);

/* TLS base (FS_BASE / TPIDR_EL0) */
void     hal_cpu_set_tls(uint64_t base);
uint64_t hal_cpu_get_tls(void);

/* FPU/SIMD state */
void hal_cpu_fpu_save(void *area);
void hal_cpu_fpu_restore(const void *area);

/* Kernel percpu base (KERNEL_GS_BASE / TPIDR_EL1) */
void hal_cpu_set_percpu_base(uint64_t base);

/* Hardware entropy. Returns 1 on success, 0 if source unavailable. */
int  hal_cpu_hwrand(uint64_t *out);

/* User-memory access window (SMAP STAC / PAN clear).
 * Must be paired strictly; no sleeping or IRQ-unsafe ops in between. */
void hal_cpu_user_access_begin(void);
void hal_cpu_user_access_end(void);

/* Platform shutdown (power-off). Implementation lives in arch+platform code. */
void hal_cpu_shutdown(void);

/* Force immediate system reset (triple-fault on x86, PSCI_SYSTEM_RESET on arm). */
void hal_cpu_reset(void);

#endif
