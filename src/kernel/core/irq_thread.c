/* CosmoRT IRQ Thread — schedulable kernel threads for device IRQ handling */

#include "core/irq_thread.h"
#include "proc/thread.h"
#include "core/percpu.h"
#include "mm/page_alloc.h"
#include "hw/serial.h"
#include "config.h"
#include "arch/arch.h"

extern void sched_add(thread_t *t);
extern void sched_wake(thread_t *t);
extern void switch_to_idle(thread_t *cur);

static void irq_thread_loop(void *arg) {
    irq_thread_t *it = (irq_thread_t *)arg;
    thread_t *self = it->thread;

    for (;;) {
        while (!__atomic_load_n(&it->pending, __ATOMIC_ACQUIRE)) {
            self->state = THREAD_BLOCKED;
            switch_to_idle(self);
        }

        __atomic_store_n(&it->pending, 0, __ATOMIC_RELEASE);
        it->handler();
    }
}

void irq_thread_create(irq_thread_t *it, const char *name, irq_thread_fn handler, int prio) {
    it->handler = handler;
    it->pending = 0;
    it->name = name;

    thread_t *t = thread_alloc();
    if (!t) {
        serial_puts("irq_thread: alloc failed for ");
        serial_puts(name);
        serial_putchar('\n');
        return;
    }

    t->state = THREAD_RUNNABLE;
    t->sched_policy = SCHED_FIFO;
    t->priority = prio;
    t->saved_priority = -1;
    t->preempt_count = 0;
    t->need_resched = 0;
    t->cpu_affinity = 0;
    t->timeslice = 0;
    t->proc = 0;

    t->kthread_fn = irq_thread_loop;
    t->kthread_arg = it;

    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) {
        serial_puts("irq_thread: stack alloc failed for ");
        serial_puts(name);
        serial_putchar('\n');
        thread_free(t);
        return;
    }
    t->kstack_top = (uint64_t)(uintptr_t)(t->kstack + KSTACK_SIZE);

    {
        extern void thread_init_kstack(thread_t *t, void (*entry)(void));
        extern void kthread_entry_trampoline(void);
        thread_init_kstack(t, kthread_entry_trampoline);
    }

    it->thread = t;

    serial_puts("irq_thread: created ");
    serial_puts(name);
    serial_putchar('\n');

    sched_add(t);
}

void irq_thread_wake(irq_thread_t *it) {
    __atomic_store_n(&it->pending, 1, __ATOMIC_RELEASE);
    sched_wake(it->thread);
}
