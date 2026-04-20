/* HAL — Interrupt controller (arch-agnostic)
 *
 * Abstracts APIC (x86) / GIC (ARM). Kernel registers handlers
 * by IRQ number, HAL routes to the correct hardware vector.
 */
#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <stdint.h>

/* irq_handler_t defined in core/irq.h (int vector signature).
 * CosmoRT handler uses the full frame via separate dispatcher -
 * no opaque data pointer needed at the HAL boundary. */
#include "core/irq.h"

/* Init interrupt controller (APIC/GIC) + IDT/vector table */
void hal_irq_init(void);

/* Register/unregister IRQ handler */
void hal_irq_register(int vector, irq_handler_t fn);
void hal_irq_unregister(int vector);

/* Enable/disable specific IRQ line */
void hal_irq_enable(int irq);
void hal_irq_disable(int irq);

/* End-of-interrupt (EOI) */
void hal_irq_eoi(void);

/* Mask all IRQs (panic path) */
void hal_irq_mask_all(void);

/* Inter-Processor Interrupt */
void hal_irq_send_ipi(int target_cpu, int vector);
void hal_irq_send_ipi_all(int vector);

/* Install vector table pointer (LIDT on x86, VBAR_EL1 on aarch64).
 * desc format is arch-private; kernel only passes an opaque token. */
void hal_irq_install_vector_table(const void *desc);

#endif
