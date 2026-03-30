/* Calibrated timer — frequency-invariant time base */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "arch/arch.h"

void timer_init(void);

uint64_t timer_ms(void);

void timer_sleep_ms(uint32_t ms);

uint32_t timer_epoch_sec(void);

extern uint64_t timer_tsc_per_ms;

extern uint64_t timer_boot_tsc;

extern uint64_t tsc_khz;

extern int tsc_invariant;

static inline uint64_t timer_tsc_now(void) {
    return arch_rdtsc();
}

static inline uint64_t timer_deadline_tsc(uint64_t ms_from_now) {
    return timer_tsc_now() + ms_from_now * timer_tsc_per_ms;
}

extern uint64_t rtc_epoch_sec;

void rtc_init(void);

#endif
