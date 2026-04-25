/* Calibrated timer — geschwindigkeitsunabhaengige Zeitbasis */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Initialize: calibrate TSC against APIC timer */
void timer_init(void);

/* Milliseconds since boot */
uint64_t timer_ms(void);

/* Sleep for ms milliseconds (busy-wait with HLT) */
void timer_sleep_ms(uint32_t ms);

/* Seconds since Unix epoch (boot RTC + uptime) */
uint32_t timer_epoch_sec(void);

/* TSC ticks per millisecond (after calibration) */
extern uint64_t timer_tsc_per_ms;

/* Boot TSC value (for absolute TSC→ms conversion) */
extern uint64_t timer_boot_tsc;

/* Hot-path TSC->ns scaling: ns = (delta_tsc * mult) >> shift.
 * Set once in timer_init via clocksource_calc_mult_shift, then read-only.
 * Avoids two integer divisions in hrtimer_now_ns(). */
extern uint32_t timer_tsc_to_ns_mult;
extern uint32_t timer_tsc_to_ns_shift;

/* Raw TSC read */
static inline uint64_t timer_tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Absolute TSC deadline for now + ms_from_now */
static inline uint64_t timer_deadline_tsc(uint64_t ms_from_now) {
    return timer_tsc_now() + ms_from_now * timer_tsc_per_ms;
}

/* Seconds since Unix epoch at boot (from CMOS RTC) */
extern int64_t rtc_epoch_sec;

/* Read CMOS RTC and set rtc_epoch_sec. Call once at boot. */
void rtc_init(void);

/* Probes nach timer_init(): Kalibrierungs-Pfad, TSC-Invariant, Frequenz. */
int      tsc_is_invariant(void);
int      tsc_calibrated_via_hpet(void);
uint64_t tsc_calibrated_hz(void);

/* Test-Hook: erzwinge Invariant-Rueckgabe. state: -1 auto, 0 not, 1 invariant. */
void     tsc_override_invariant_for_test(int state);

/* Test-Hook: fuehre Re-Kalibrierung aus (PIT oder HPET, je nach hpet_available). */
void     tsc_recalibrate_for_test(void);

/* Test-Hook: HPET-Kalibrierungs-Pfad isoliert aufrufen. 0 bei Timeout/HPET fehlt. */
uint64_t tsc_calibrate_via_hpet_for_test(void);

#endif
