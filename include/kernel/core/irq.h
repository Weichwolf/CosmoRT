/* Interrupt handling — APIC + IDT */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

void irq_init(void);

typedef void (*irq_handler_t)(int vector);
void irq_register(int vector, irq_handler_t handler);

static inline void irq_enable(void)  { __asm__ volatile("sti"); }
static inline void irq_disable(void) { __asm__ volatile("cli"); }

extern volatile int irq_event_pending;

uint64_t irq_get_ticks(void);

void lapic_eoi(void);

void ioapic_route_irq(uint8_t irq, uint8_t vector);

void tlb_shootdown(uint64_t pml4_phys);

void lapic_delay_ms(uint32_t ms);

#endif
