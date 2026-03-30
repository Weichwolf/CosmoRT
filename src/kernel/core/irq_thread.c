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
extern int  kernel_setjmp(uint64_t buf[8]);
extern void kernel_longjmp(uint64_t buf[8], int val) __attribute__((noreturn));
extern uint64_t pml4[];

static void irq_thread_loop(void *arg) {
    irq_thread_t *it = (irq_thread_t *)arg;
    thread_t *self = it->thread;

    for (;;) {
        while (!__atomic_load_n(&it->pending, __ATOMIC_ACQUIRE)) {
            self->state = THREAD_BLOCKED;
            if (kernel_setjmp(self->kernel_yield_jmpbuf) == 0) {
                self->in_kernel_yield = 1;
                arch_set_cr3(virt_to_phys(pml4));
                kernel_longjmp(self->jmpbuf, 1);
            }
            if (__atomic_load_n(&it->pending, __ATOMIC_ACQUIRE))
                break;
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
