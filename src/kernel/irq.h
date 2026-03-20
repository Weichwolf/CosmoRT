/* Interrupt handling — APIC + IDT */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/* Initialize Local APIC + I/O APIC + IDT */
void irq_init(void);

/* Register a handler for an IRQ vector (32-255) */
typedef void (*irq_handler_t)(int vector);
void irq_register(int vector, irq_handler_t handler);

/* Enable/disable interrupts */
static inline void irq_enable(void)  { __asm__ volatile("sti"); }
static inline void irq_disable(void) { __asm__ volatile("cli"); }

/* Signal that an event occurred (wakes HLT) */
extern volatile int irq_event_pending;

/* Timer ticks since boot */
uint64_t irq_get_ticks(void);

#endif
