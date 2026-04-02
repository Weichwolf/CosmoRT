/* HAL — Timer (arch-agnostic)
 *
 * Abstracts LAPIC timer (x86) / Generic Timer (ARM).
 * Used by hrtimer subsystem for one-shot deadline programming.
 */
#ifndef HAL_TIMER_H
#define HAL_TIMER_H

#include <stdint.h>

/* Initialize hardware timer (calibrate, set up IRQ) */
void hal_timer_init(void);

/* One-shot: fire IRQ after delta_ns nanoseconds */
void hal_timer_set_oneshot(uint64_t delta_ns);

/* Periodic: fire IRQ every period_ns nanoseconds */
void hal_timer_set_periodic(uint64_t period_ns);

/* Disarm: stop timer, no more IRQs until next set */
void hal_timer_disarm(void);

/* Monotonic nanosecond clock (TSC / CNTVCT based) */
uint64_t hal_timer_now_ns(void);

/* Ticks per millisecond (for legacy callers during migration) */
uint64_t hal_timer_ticks_per_ms(void);

#endif
