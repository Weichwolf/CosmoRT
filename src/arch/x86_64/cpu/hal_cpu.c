/* x86_64 HAL - CPU primitives */

#include "hal/hal_cpu.h"
#include "arch/arch.h"
#include "core/percpu.h"
#include "config.h"

int hal_cpu_id(void) {
    return percpu_self()->core_id;
}

int hal_cpu_count(void) {
    extern int smp_num_cores(void);
    return smp_num_cores();
}

void hal_cpu_halt(void)        { arch_halt(); }
void hal_cpu_halt_noirq(void)  { arch_cli_halt(); }
void hal_cpu_relax(void)       { arch_pause(); }
void hal_cpu_cli(void)         { arch_cli(); }
void hal_cpu_sti(void)         { arch_sti(); }

uint64_t hal_cpu_save_irq(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void hal_cpu_restore_irq(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "g"(flags) : "memory", "cc");
}

void hal_cpu_mfence(void) { arch_mfence(); }
void hal_cpu_wmb(void)    { arch_wmb(); }
void hal_cpu_rmb(void)    { arch_rmb(); }

void hal_cpu_store_release(volatile uint32_t *p, uint32_t v) { arch_store_release(p, v); }
uint32_t hal_cpu_load_acquire(volatile uint32_t *p)          { return arch_load_acquire(p); }

uint64_t hal_cpu_timestamp(void) { return arch_rdtsc(); }
uint64_t hal_cpu_stack_ptr(void) { return arch_get_rsp(); }

void     hal_cpu_set_tls(uint64_t base) { arch_set_fs_base(base); }
uint64_t hal_cpu_get_tls(void)          { return arch_get_fs_base(); }

void hal_cpu_fpu_save(void *area)           { arch_fpstate_save(area); }
void hal_cpu_fpu_restore(const void *area)  { arch_fpstate_restore(area); }

void hal_cpu_set_percpu_base(uint64_t base) { arch_set_kernel_gs_base(base); }

/* Set GS_BASE directly (active in kernel mode before first swapgs). */
void hal_cpu_set_percpu_active(uint64_t base) {
    arch_wrmsr(0xC0000101, base);  /* IA32_GS_BASE */
}

void     hal_io_outb(uint16_t port, uint8_t val)   { arch_outb(port, val); }
uint8_t  hal_io_inb(uint16_t port)                 { return arch_inb(port); }
void     hal_io_outl(uint16_t port, uint32_t val)  { arch_outl(port, val); }
uint32_t hal_io_inl(uint16_t port)                 { return arch_inl(port); }

int hal_cpu_hwrand(uint64_t *out) {
    extern int memops_has_rdrand;
    if (!memops_has_rdrand) return 0;
    return arch_rdrand_checked(out);
}

void hal_cpu_user_access_begin(void) { arch_stac(); }
void hal_cpu_user_access_end(void)   { arch_clac(); }

void hal_cpu_shutdown(void) { arch_shutdown(); }

/* Reset via triple-fault: load zero-length IDT, then trigger an INT3.
 * With no valid IDT, CPU takes #UD -> #DF -> triple fault -> reset. */
void hal_cpu_reset(void) {
    struct __attribute__((packed)) { uint16_t l; uint64_t b; } null_idt = {0, 0};
    arch_cli();
    __asm__ volatile("lidt %0" :: "m"(null_idt));
    __asm__ volatile("int3");
    arch_cli_halt();
}
