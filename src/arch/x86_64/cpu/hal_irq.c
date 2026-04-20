/* x86_64 HAL - IRQ subsystem glue.
 *
 * irq_init, APIC programming, IDT construction currently live in
 * src/kernel/core/irq.c - that code is x86-specific and will move here
 * in a future pass. For now this file only carries the vector-table
 * install primitive so the caller stops seeing arch_* directly.
 */

#include "hal/hal_irq.h"
#include "arch/arch.h"

void hal_irq_install_vector_table(const void *desc) {
    arch_lidt(desc);
}
