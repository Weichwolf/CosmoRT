/* SMP — stub for single-core phase.
 * Provides smp_num_cores(), smp_core_id(), rt_wake() as safe no-ops.
 */

#include "config.h"
#include "core/smp.h"
#include "arch/arch.h"

#define LAPIC_ID      (0xFEE00020ULL + PHYS_OFFSET)
#define LAPIC_ICR_LO  (0xFEE00300ULL + PHYS_OFFSET)
#define LAPIC_ICR_HI  (0xFEE00310ULL + PHYS_OFFSET)

int smp_num_cores(void) { return 1; }

int smp_core_running(int core) { return core == 0; }

int smp_core_id(void) {
    volatile uint32_t *lapic_id = (volatile uint32_t *)LAPIC_ID;
    return (int)((*lapic_id >> 24) & 0xFF);
}

void rt_wake(int core_id) {
    (void)core_id; /* single-core: no other cores to wake */
}
