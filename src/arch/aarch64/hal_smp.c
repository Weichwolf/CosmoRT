/* aarch64 HAL - SMP (stubs).
 *
 * Phase-9 placeholder. Real impl will drive PSCI for CPU_ON + bring APs
 * up through the secondary entry trampoline in boot/.
 *
 * Not linked into the x86_64 build.
 */

#include "hal/hal_smp.h"
#include "kernel/panic.h"

#define AARCH64_STUB(name) \
    do { (void)0; panic("aarch64 hal_smp_" #name " not implemented"); } while (0)

int hal_smp_boot_ap(int cpu, void (*entry)(void), void *stack) {
    (void)cpu; (void)entry; (void)stack;
    AARCH64_STUB(boot_ap); return -1;
}
int hal_smp_num_cpus(void)       { AARCH64_STUB(num_cpus); return 1; }
void *hal_smp_percpu(int cpu)    { (void)cpu; AARCH64_STUB(percpu); return 0; }
