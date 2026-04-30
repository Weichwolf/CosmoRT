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

/* Per-thread user GS base.
 * x86: IA32_KERNEL_GS_BASE while in kernel — swapgs makes it active in user mode.
 * aarch64: TPIDRRO_EL0. Save/restore on context switch. */
void     hal_cpu_set_user_gs(uint64_t base);
uint64_t hal_cpu_get_user_gs(void);

/* Validate an address is acceptable for TLS-base writes (FS_BASE / GS_BASE).
 * x86: bits 48-63 must equal bit 47 (canonical form); wrmsr with non-canonical
 * triggers #GP. aarch64: any 64-bit value. Returns nonzero if the address is
 * acceptable, 0 if it must be rejected at the syscall boundary. */
int      hal_cpu_canonical_user_addr(uint64_t addr);

/* Architecture name for uname.machine ("x86_64", "aarch64", ...). */
const char *hal_cpu_arch_name(void);

/* FPU/SIMD state */
void hal_cpu_fpu_save(void *area);
void hal_cpu_fpu_restore(const void *area);

/* Kernel percpu base.
 * x86: KERNEL_GS_BASE (loaded by swapgs on syscall entry).
 * aarch64: TPIDR_EL1. */
void hal_cpu_set_percpu_base(uint64_t base);

/* Active percpu base - read immediately by the running core.
 * x86: GS_BASE (swapped with KERNEL_GS_BASE). Must be primed at BSP init
 * before the first swapgs so percpu_self() works in early boot. */
void hal_cpu_set_percpu_active(uint64_t base);

/* Hardware entropy. Returns 1 on success, 0 if source unavailable. */
int  hal_cpu_hwrand(uint64_t *out);

/* One-shot boot init: CPU feature detection + FPU/SIMD enablement.
 * x86: enables SSE, probes/enables XSAVE, configures XCR0, enables
 * SMEP/SMAP. aarch64: enables FP/SIMD via CPACR_EL1, probes SVE. */
void hal_cpu_fpu_boot_init(void);

/* User-memory access window (SMAP STAC / PAN clear).
 * Must be paired strictly; no sleeping or IRQ-unsafe ops in between. */
void hal_cpu_user_access_begin(void);
void hal_cpu_user_access_end(void);

/* Platform shutdown (power-off). Implementation lives in arch+platform code. */
void hal_cpu_shutdown(void);

/* Force immediate system reset (triple-fault on x86, PSCI_SYSTEM_RESET on arm). */
void hal_cpu_reset(void);

/* Port I/O (x86 separate I/O space).
 * On aarch64 these map to MMIO accesses at an architecture-specific base.
 * Kernel drivers must not assume one or the other. */
void     hal_io_outb(uint16_t port, uint8_t val);
uint8_t  hal_io_inb(uint16_t port);
void     hal_io_outw(uint16_t port, uint16_t val);
uint16_t hal_io_inw(uint16_t port);
void     hal_io_outl(uint16_t port, uint32_t val);
uint32_t hal_io_inl(uint16_t port);

#endif
