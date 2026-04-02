/* x86_64 HAL — CPU primitives */

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

void hal_cpu_halt(void)  { arch_sti(); __asm__ volatile("hlt"); }
void hal_cpu_relax(void) { arch_pause(); }
void hal_cpu_cli(void)   { arch_cli(); }
void hal_cpu_sti(void)   { arch_sti(); }

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

uint64_t hal_cpu_timestamp(void) { return arch_rdtsc(); }

void     hal_cpu_set_tls(uint64_t base) { arch_set_fs_base(base); }
uint64_t hal_cpu_get_tls(void)          { return arch_get_fs_base(); }

void hal_cpu_fpu_save(void *area)           { arch_fpstate_save(area); }
void hal_cpu_fpu_restore(const void *area)  { arch_fpstate_restore(area); }

void hal_cpu_set_percpu_base(uint64_t base) { arch_set_kernel_gs_base(base); }
