/* x86_64 HAL — Timer (LAPIC + TSC) */

#include "hal/hal_timer.h"
#include "core/timer.h"
#include "core/hrtimer.h"
#include "core/clocksource.h"

void hal_timer_init(void) {
    timer_init();       /* TSC calibration */
    extern void rtc_init(void);
    rtc_init();
    hrtimer_init_subsystem(); /* LAPIC calibration + one-shot setup */
}

void hal_timer_set_oneshot(uint64_t delta_ns) {
    /* hrtimer subsystem handles LAPIC programming */
    (void)delta_ns;
}

void hal_timer_set_periodic(uint64_t period_ns) {
    (void)period_ns;
    /* Currently handled by irq_init's LAPIC setup */
}

void hal_timer_disarm(void) {
    /* hrtimer_reprogram handles this */
}

uint64_t hal_timer_now_ns(void) {
    return clocksource_read_ns();
}

uint64_t hal_timer_ticks_per_ms(void) {
    return timer_tsc_per_ms;
}
