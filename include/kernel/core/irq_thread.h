/* CosmoRT IRQ Thread — schedulable kernel threads for device IRQ handling */
#ifndef IRQ_THREAD_H
#define IRQ_THREAD_H

#include "proc/thread.h"

typedef void (*irq_thread_fn)(void);

typedef struct irq_thread {
    thread_t      *thread;
    irq_thread_fn  handler;
    volatile int   pending;
    const char    *name;
} irq_thread_t;

void irq_thread_create(irq_thread_t *it, const char *name, irq_thread_fn handler, int prio);
void irq_thread_wake(irq_thread_t *it);

#endif
