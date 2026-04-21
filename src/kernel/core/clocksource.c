/* CosmoRT clocksource / clock_event_device core
 *
 * Linked-list registry, sorted by rating (descending). current = head.
 * HAL reads monotonic ns via clocksource_read_ns(), arms one-shots via
 * clock_event_set_oneshot().
 */

#include "core/clocksource.h"
#include "spinlock.h"

static struct clocksource       *cs_head;
static struct clock_event_device *ce_head;
static spinlock_t                 cs_lock = SPINLOCK_INIT;
static spinlock_t                 ce_lock = SPINLOCK_INIT;

/* Wrap accounting: (cycles - last) & mask accumulates into ns_base so
 * narrow counters (24/32-bit) don't fold. Read path is lock-free on the
 * current pointer; consistency comes from 128-bit-split via paired u64
 * snapshot inside the current clocksource wrapper. For 64-bit counters
 * (TSC) wrap is irrelevant in any realistic uptime. */
static uint64_t cs_cycle_last;
static uint64_t cs_ns_base;

/* Compute mult/shift for `from_hz -> to_hz` conversion. Pick the largest
 * shift such that
 *   (from_hz * max_sec) << shift  <=  UINT64_MAX
 * i.e. the accumulator can hold `max_sec` seconds worth of cycles without
 * overflowing when shifted. Mirrors Linux clocks_calc_mult_shift. */
void clocksource_calc_mult_shift(uint32_t *mult, uint32_t *shift,
                                 uint64_t from_hz, uint64_t to_hz,
                                 uint32_t max_sec) {
    uint64_t tmp;
    uint32_t sft, sftacc = 32;

    tmp = ((uint64_t)max_sec * from_hz) >> 32;
    while (tmp) { tmp >>= 1; sftacc--; }

    for (sft = 32; sft > 0; sft--) {
        tmp = (uint64_t)to_hz << sft;
        tmp += from_hz / 2;
        tmp /= from_hz;
        if ((tmp >> sftacc) == 0)
            break;
    }
    *mult  = (uint32_t)tmp;
    *shift = sft;
}

/* Insert sorted by rating, highest first. */
static void cs_insert_locked(struct clocksource *cs) {
    struct clocksource **link = &cs_head;
    while (*link && (*link)->rating >= cs->rating)
        link = &(*link)->next;
    cs->next = *link;
    *link = cs;
}

void clocksource_register(struct clocksource *cs) {
    uint64_t flags;
    spin_lock_irq(&cs_lock, &flags);

    int became_current = (cs_head == 0) || cs->rating > cs_head->rating;
    cs_insert_locked(cs);

    if (became_current && cs_head == cs) {
        cs_cycle_last = cs->read() & cs->mask;
        cs_ns_base    = 0;
    }
    spin_unlock_irq(&cs_lock, flags);
}

void clocksource_unregister(struct clocksource *cs) {
    uint64_t flags;
    spin_lock_irq(&cs_lock, &flags);

    struct clocksource **link = &cs_head;
    while (*link && *link != cs) link = &(*link)->next;
    if (*link) *link = cs->next;
    cs->next = 0;

    if (cs_head) {
        cs_cycle_last = cs_head->read() & cs_head->mask;
        cs_ns_base    = 0;
    }
    spin_unlock_irq(&cs_lock, flags);
}

struct clocksource *clocksource_current(void) { return cs_head; }

uint64_t clocksource_read_ns(void) {
    uint64_t flags;
    spin_lock_irq(&cs_lock, &flags);

    struct clocksource *cs = cs_head;
    if (!cs) { spin_unlock_irq(&cs_lock, flags); return 0; }

    uint64_t cycles = cs->read() & cs->mask;
    uint64_t delta  = (cycles - cs_cycle_last) & cs->mask;
    uint64_t ns     = cs_ns_base + (((delta * cs->mult) >> cs->shift));
    cs_cycle_last   = cycles;
    cs_ns_base      = ns;

    spin_unlock_irq(&cs_lock, flags);
    return ns;
}

/* ── clock_event_device ─────────────────────────────── */

static void ce_insert_locked(struct clock_event_device *ce) {
    struct clock_event_device **link = &ce_head;
    while (*link && (*link)->rating >= ce->rating)
        link = &(*link)->next;
    ce->next = *link;
    *link = ce;
}

void clock_event_register(struct clock_event_device *ce) {
    uint64_t flags;
    spin_lock_irq(&ce_lock, &flags);
    ce_insert_locked(ce);
    spin_unlock_irq(&ce_lock, flags);
}

struct clock_event_device *clock_event_current(void) { return ce_head; }

int clock_event_set_oneshot(uint64_t delta_ns) {
    struct clock_event_device *ce = ce_head;
    if (!ce || !ce->set_next_event) return -1;
    return ce->set_next_event(delta_ns);
}

int clock_event_set_periodic(uint64_t period_ns) {
    struct clock_event_device *ce = ce_head;
    if (!ce || !ce->set_periodic) return -1;
    return ce->set_periodic(period_ns);
}

int clock_event_shutdown(void) {
    struct clock_event_device *ce = ce_head;
    if (!ce || !ce->shutdown) return -1;
    return ce->shutdown();
}
