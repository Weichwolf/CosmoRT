/* HAL — Interrupt controller (arch-agnostic)
 *
 * Abstracts APIC (x86) / GIC (ARM). Kernel registers handlers
 * by IRQ number, HAL routes to the correct hardware vector.
 */
#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <stdint.h>

typedef void (*irq_handler_t)(int irq, void *data);

/* Init interrupt controller (APIC/GIC) + IDT/vector table */
void hal_irq_init(void);

/* Register/unregister IRQ handler */
void hal_irq_register(int irq, irq_handler_t fn, void *data);
void hal_irq_unregister(int irq);

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

#endif
