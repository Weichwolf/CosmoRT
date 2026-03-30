/* CosmoRT Spinlock — compatibility layer over raw_spinlock_t
 *
 * All spinlock_t usage maps directly to raw_spinlock_t.
 * For sleeping locks with priority inheritance, use mutex_t.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "core/raw_spinlock.h"

typedef raw_spinlock_t spinlock_t;

#define SPINLOCK_INIT RAW_SPINLOCK_INIT

#define spin_lock       raw_spin_lock
#define spin_unlock     raw_spin_unlock
#define spin_trylock    raw_spin_trylock

#define irq_save        raw_irq_save
#define irq_restore     raw_irq_restore

#define spin_lock_irq   raw_spin_lock_irq
#define spin_unlock_irq raw_spin_unlock_irq
#define spin_trylock_irq raw_spin_trylock_irq

#endif
