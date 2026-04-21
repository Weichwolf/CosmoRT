/* CosmoRT clocksource / clock_event_device core abstraction
 *
 * Linux-ABI-compatible rating scale:
 *   0-1    Device not recommended
 *   1-99   Fallback
 *   100-199 Correct and usable
 *   200-299 Good
 *   300-399 Desired
 *   400-499 Ideal (VM paravirt, invariant TSC)
 *
 * HAL-API sits above: hal_timer_now_ns -> clocksource_read_ns,
 * hal_timer_set_oneshot -> clock_event_set_oneshot.
 */
#ifndef CORE_CLOCKSOURCE_H
#define CORE_CLOCKSOURCE_H

#include <stdint.h>

#define CLOCKSOURCE_RATING_FALLBACK    1
#define CLOCKSOURCE_RATING_BASE        100
#define CLOCKSOURCE_RATING_ACPI_PM     110
#define CLOCKSOURCE_RATING_HPET        250
#define CLOCKSOURCE_RATING_TSC         300
#define CLOCKSOURCE_RATING_KVM_PVCLOCK 400
#define CLOCKSOURCE_RATING_MAX         499

#define CLOCKSOURCE_FLAG_CONTINUOUS     0x01
#define CLOCKSOURCE_FLAG_SUSPEND_NONSTOP 0x02
#define CLOCKSOURCE_FLAG_MUST_VERIFY    0x04
#define CLOCKSOURCE_FLAG_VALID_FOR_HRES 0x20

#define CLOCK_EVT_FEAT_ONESHOT   0x01
#define CLOCK_EVT_FEAT_PERIODIC  0x02
#define CLOCK_EVT_FEAT_SHUTDOWN  0x04

#define CLOCKSOURCE_MASK_64     0xffffffffffffffffULL
#define CLOCKSOURCE_MASK_32     0x00000000ffffffffULL
#define CLOCKSOURCE_MASK_24     0x0000000000ffffffULL

struct clocksource {
    const char        *name;
    int                rating;
    uint64_t         (*read)(void);
    uint64_t           mask;
    uint32_t           mult;
    uint32_t           shift;
    uint32_t           flags;
    struct clocksource *next;
};

struct clock_event_device {
    const char *name;
    int         rating;
    uint32_t    features;
    int       (*set_next_event)(uint64_t delta_ns);
    int       (*set_periodic)(uint64_t period_ns);
    int       (*shutdown)(void);
    struct clock_event_device *next;
};

void        clocksource_register(struct clocksource *cs);
void        clocksource_unregister(struct clocksource *cs);
uint64_t    clocksource_read_ns(void);
struct clocksource *clocksource_current(void);

void        clock_event_register(struct clock_event_device *ce);
int         clock_event_set_oneshot(uint64_t delta_ns);
int         clock_event_set_periodic(uint64_t period_ns);
int         clock_event_shutdown(void);
struct clock_event_device *clock_event_current(void);

/* Derive mult/shift from counter frequency so that
 *   ns = ((cycles & mask) * mult) >> shift
 * max_sec bounds the shift so (freq * max_sec) fits in 64 bits after shifting.
 * Modeled after Linux kernel/time/clocksource.c:clocks_calc_mult_shift. */
void clocksource_calc_mult_shift(uint32_t *mult, uint32_t *shift,
                                 uint64_t from_hz, uint64_t to_hz,
                                 uint32_t max_sec);

/* Convenience: frequency in Hz -> mult/shift for ns conversion. */
static inline void clocksource_hz_to_mult(struct clocksource *cs, uint64_t hz,
                                          uint32_t max_sec) {
    clocksource_calc_mult_shift(&cs->mult, &cs->shift, hz, 1000000000ULL, max_sec);
}

#endif
