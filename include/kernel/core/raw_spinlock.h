/* CosmoRT Raw Spinlock — ticket lock for SMP safety
 *
 * Always used with IRQ disable: raw_spin_lock_irq / raw_spin_unlock_irq.
 * Bare raw_spin_lock/raw_spin_unlock for contexts where IRQs are already disabled.
 *
 * This is the actual hardware spinlock. For sleeping locks, use mutex_t.
 */
#ifndef RAW_SPINLOCK_H
#define RAW_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t next;
    volatile uint32_t owner;
} raw_spinlock_t;

#define RAW_SPINLOCK_INIT {0, 0}

static inline void raw_spin_lock(raw_spinlock_t *l) {
    uint32_t ticket = __sync_fetch_and_add(&l->next, 1);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket)
        __asm__ volatile("pause");
}

static inline void raw_spin_unlock(raw_spinlock_t *l) {
    __asm__ volatile("" ::: "memory"); /* compiler barrier */
    __sync_fetch_and_add(&l->owner, 1);
}

static inline int raw_spin_trylock(raw_spinlock_t *l) {
    uint32_t cur = l->owner;
    return __sync_bool_compare_and_swap(&l->next, cur, cur + 1);
}

/* Lock with IRQ save/disable */
static inline uint64_t raw_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void raw_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
}

static inline void raw_spin_lock_irq(raw_spinlock_t *l, uint64_t *flags) {
    *flags = raw_irq_save();
    raw_spin_lock(l);
}

static inline void raw_spin_unlock_irq(raw_spinlock_t *l, uint64_t flags) {
    raw_spin_unlock(l);
    raw_irq_restore(flags);
}

static inline int raw_spin_trylock_irq(raw_spinlock_t *l, uint64_t *flags) {
    *flags = raw_irq_save();
    if (raw_spin_trylock(l)) return 1;
    raw_irq_restore(*flags);
    return 0;
}

#endif
