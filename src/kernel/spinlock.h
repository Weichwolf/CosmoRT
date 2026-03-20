/* CosmoRT Spinlock — ticket lock for SMP safety
 *
 * Always used with IRQ disable: spin_lock_irq / spin_unlock_irq.
 * Bare spin_lock/spin_unlock for contexts where IRQs are already disabled.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t next;
    volatile uint32_t owner;
} spinlock_t;

#define SPINLOCK_INIT {0, 0}

static inline void spin_lock(spinlock_t *l) {
    uint32_t ticket = __sync_fetch_and_add(&l->next, 1);
    while (__sync_val_compare_and_swap(&l->owner, ticket, ticket) != ticket)
        __asm__ volatile("pause");
}

static inline void spin_unlock(spinlock_t *l) {
    __asm__ volatile("" ::: "memory"); /* compiler barrier */
    __sync_fetch_and_add(&l->owner, 1);
}

static inline int spin_trylock(spinlock_t *l) {
    uint32_t cur = l->owner;
    return __sync_bool_compare_and_swap(&l->next, cur, cur + 1);
}

/* Lock with IRQ save/disable */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
}

static inline void spin_lock_irq(spinlock_t *l, uint64_t *flags) {
    *flags = irq_save();
    spin_lock(l);
}

static inline void spin_unlock_irq(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    irq_restore(flags);
}

#endif
