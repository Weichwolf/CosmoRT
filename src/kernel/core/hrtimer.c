/* CosmoRT High-Resolution Timer — RB-tree + LAPIC one-shot
 *
 * All kernel timeouts go through hrtimer_start/cancel.
 * Timer IRQ fires only when a deadline expires — no periodic tick.
 * LAPIC one-shot is reprogrammed to the nearest deadline after each change.
 */

#include "core/hrtimer.h"
#include "core/rbtree.h"
#include "core/timer.h"
#include "arch/arch.h"
#include "hal/hal.h"
#include "hw/serial.h"
#include "spinlock.h"

/* LAPIC registers for one-shot timer */
#define LAPIC_BASE        0xFEE00000ULL
#define LAPIC_TIMER_LVT   0x320
#define LAPIC_TIMER_INIT   0x380
#define LAPIC_TIMER_CUR    0x390
#define LAPIC_TIMER_DIV    0x3E0
#define LAPIC_TIMER_VECTOR  32
/* LVT: one-shot mode = bit 17 clear, periodic = bit 17 set */
#define LVT_ONESHOT       (LAPIC_TIMER_VECTOR)         /* one-shot, vector 32 */
#define LVT_MASKED        (LAPIC_TIMER_VECTOR | 0x10000)

static inline void lapic_write(uint32_t reg, uint32_t val) {
    volatile uint32_t *p = (volatile uint32_t *)(LAPIC_BASE + 0xFFFF800000000000ULL + reg);
    *p = val;
}

static inline uint32_t lapic_read(uint32_t reg) {
    volatile uint32_t *p = (volatile uint32_t *)(LAPIC_BASE + 0xFFFF800000000000ULL + reg);
    return *p;
}

/* Per-CPU base (single-core for now) */
static hrtimer_base_t base;

/* LAPIC ticks per millisecond (calibrated at init) */
static uint64_t lapic_ticks_per_ms;

/* Minimum LAPIC one-shot value (avoid zero-length timer) */
#define LAPIC_MIN_TICKS 100

/* Tick-less mode: LAPIC laeuft im one-shot, deadline ist immer
 * min(next-hrtimer, next-periodic-tick). next_periodic_tick_ns wird in
 * timer_handler nach jedem Fire re-armiert. Periodischer 1000Hz-Tick
 * bleibt fuer Legacy-tick_run/sched_preempt erhalten — Subsysteme sehen
 * nichts vom Mode-Wechsel. Phase 12.x. */
#define TICK_PERIOD_NS 1000000ULL  /* 1ms */
static uint64_t next_periodic_tick_ns;
/* Set in init nach LAPIC-Calibration. 0 vorher -> reprogram Noop. */
static int tickless_active;

/* ── Monotonic clock ───────────────────────────────── */

uint64_t hrtimer_now_ns(void) {
    /* Slow path nur vor timer_init: Mult ist noch 0, kein TSC-Read sinnvoll. */
    if (__builtin_expect(timer_tsc_to_ns_mult == 0, 0)) return 0;
    uint64_t tsc = timer_tsc_now();
    uint64_t delta = tsc - timer_boot_tsc;
    /* (delta * mult) >> shift — single mul, single shift; selbe Praezision wie
     * clocksource_read_ns ohne dessen Lock. delta*mult kann fuer ~600s @4GHz
     * nicht ueberlaufen (TSC_MULT_MAX_SEC_WINDOW=600 garantiert das). */
    return (delta * (uint64_t)timer_tsc_to_ns_mult) >> timer_tsc_to_ns_shift;
}

/* ── LAPIC one-shot programming ────────────────────── */

static void lapic_arm_ns(uint64_t delta_ns) {
    /* Convert ns to LAPIC ticks. Overflow-safe: cap delta_ns auf
     * Werte die ohne overflow konvertierbar sind. Max LAPIC-Init-Wert
     * ist 32-bit (0xFFFFFFFF Ticks). */
    uint64_t ticks;
    /* delta_ns * lapic_ticks_per_ms wird in 64-bit zu gross wenn delta_ns
     * etwa > 2^64 / lapic_ticks_per_ms. Bei 60000 ticks/ms (~3GHz CPU/8):
     * delta_ns < 3e14 ns = 300_000 sec = ~3 Tage. Cap defensiv bei 1h. */
    const uint64_t MAX_DELTA_NS = 3600ULL * NSEC_PER_SEC;
    if (delta_ns > MAX_DELTA_NS) delta_ns = MAX_DELTA_NS;
    ticks = (delta_ns * lapic_ticks_per_ms) / NSEC_PER_MSEC;
    if (ticks < LAPIC_MIN_TICKS) ticks = LAPIC_MIN_TICKS;
    if (ticks > 0xFFFFFFFFULL) ticks = 0xFFFFFFFFULL;

    lapic_write(LAPIC_TIMER_LVT, LVT_ONESHOT);
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)ticks);
}

__attribute__((unused))
static void lapic_disarm(void) {
    lapic_write(LAPIC_TIMER_LVT, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0);
}

/* ── RB-tree insert/remove ─────────────────────────── */

static void enqueue(hrtimer_t *t) {
    rb_root_t *root = &base.tree;
    rb_node_t *parent = 0;
    rb_node_t **link = &root->node;
    int leftmost = 1;

    while (*link) {
        parent = *link;
        hrtimer_t *entry = rb_entry(parent, hrtimer_t, node);
        if (t->deadline_ns < entry->deadline_ns) {
            link = &parent->left;
        } else {
            link = &parent->right;
            leftmost = 0;
        }
    }

    rb_link_node(&t->node, parent, link);
    rb_insert_fixup(root, &t->node);

    if (leftmost)
        root->leftmost = &t->node;

    t->state = HRTIMER_ENQUEUED;
}

static void dequeue(hrtimer_t *t) {
    rb_erase(&base.tree, &t->node);
    t->state = HRTIMER_INACTIVE;
}

/* ── Public API ────────────────────────────────────── */

void hrtimer_init_subsystem(void) {
    base.tree = (rb_root_t)RB_ROOT_INIT;
    base.lock = (spinlock_t)SPINLOCK_INIT;

    /* Calibrate LAPIC timer: program a known interval, measure ticks.
     * Use TSC as reference (already calibrated by timer_init). */
    lapic_write(LAPIC_TIMER_DIV, 0x03); /* divide by 8 */
    lapic_write(LAPIC_TIMER_LVT, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    /* Wait ~10ms using TSC */
    uint64_t tsc_start = timer_tsc_now();
    uint64_t tsc_10ms = timer_tsc_per_ms * 10;
    while (timer_tsc_now() - tsc_start < tsc_10ms)
        hal_cpu_relax();

    uint32_t cur = lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0); /* stop */
    uint32_t elapsed = 0xFFFFFFFF - cur;

    if (elapsed < 100) {
        /* LAPIC timer didn't count — use fallback from irq_init's config */
        lapic_ticks_per_ms = 100000;
        serial_puts("hrtimer: LAPIC calibration failed, using fallback\n");
    } else {
        lapic_ticks_per_ms = elapsed / 10;
        if (lapic_ticks_per_ms == 0) lapic_ticks_per_ms = 100000;
    }

    serial_puts("hrtimer: LAPIC ");
    { uint64_t v = lapic_ticks_per_ms; char b[12]; int i = 0;
      do { b[i++] = '0' + v % 10; v /= 10; } while (v);
      while (i--) serial_putchar(b[i]); }
    serial_puts(" ticks/ms, tickless one-shot mode\n");

    /* Tick-less aktivieren: LAPIC im one-shot, Deadline = next-periodic-tick.
     * hrtimer_start() kann frueher reprogrammieren. timer_handler
     * re-armiert nach jedem Fire. */
    next_periodic_tick_ns = hrtimer_now_ns() + TICK_PERIOD_NS;
    tickless_active = 1;
    lapic_write(LAPIC_TIMER_DIV, 0x03);
    lapic_arm_ns(TICK_PERIOD_NS);
}

void hrtimer_init(hrtimer_t *t, void (*fn)(hrtimer_t *), void *data) {
    t->fn = fn;
    t->data = data;
    t->deadline_ns = 0;
    t->state = HRTIMER_INACTIVE;
    t->node.left = t->node.right = t->node.parent = 0;
}

void hrtimer_start(hrtimer_t *t, uint64_t deadline_ns) {
    uint64_t flags;
    spin_lock_irq(&base.lock, &flags);

    if (t->state == HRTIMER_ENQUEUED)
        dequeue(t);

    t->deadline_ns = deadline_ns;
    enqueue(t);

    spin_unlock_irq(&base.lock, flags);

    hrtimer_reprogram();
}

int hrtimer_cancel(hrtimer_t *t) {
    uint64_t flags;
    spin_lock_irq(&base.lock, &flags);

    int was_active = (t->state == HRTIMER_ENQUEUED);
    if (was_active)
        dequeue(t);

    spin_unlock_irq(&base.lock, flags);

    if (was_active)
        hrtimer_reprogram();

    return was_active;
}

/* Drop every enqueued timer whose data pointer matches. Needed when a thread
 * dies while blocked: its stack-allocated hrtimer_t would otherwise stay in
 * the tree pointing into freed kstack memory.
 * Restart-from-leftmost after each removal — rb_next() is invalidated by
 * dequeue() because the tree shape changes. */
int hrtimer_cancel_by_data(void *data) {
    int removed = 0;
    uint64_t flags;
    spin_lock_irq(&base.lock, &flags);

    for (;;) {
        rb_node_t *n = base.tree.leftmost;
        hrtimer_t *hit = 0;
        while (n) {
            hrtimer_t *t = rb_entry(n, hrtimer_t, node);
            if (t->data == data) { hit = t; break; }
            n = rb_next(n);
        }
        if (!hit) break;
        dequeue(hit);
        removed++;
    }

    spin_unlock_irq(&base.lock, flags);

    if (removed)
        hrtimer_reprogram();
    return removed;
}

/* ── IRQ handler: fire expired, reprogram ──────────── */

void hrtimer_run_expired(void) {
    uint64_t now = hrtimer_now_ns();

    for (;;) {
        uint64_t flags;
        spin_lock_irq(&base.lock, &flags);

        rb_node_t *leftmost = base.tree.leftmost;
        if (!leftmost) {
            spin_unlock_irq(&base.lock, flags);
            break;
        }

        hrtimer_t *t = rb_entry(leftmost, hrtimer_t, node);
        if (t->deadline_ns > now) {
            spin_unlock_irq(&base.lock, flags);
            break;
        }

        /* Timer expired — remove and fire */
        dequeue(t);
        spin_unlock_irq(&base.lock, flags);

        t->fn(t);
    }

    /* Re-armiere periodischen Tick relativ zur jetzigen Zeit. Wenn der
     * letzte Tick wegen langer hrtimer-Arbeit schon ueberfaellig ist,
     * setze sofort eine kuerzere Deadline — der naechste IRQ holt nach. */
    next_periodic_tick_ns = hrtimer_now_ns() + TICK_PERIOD_NS;
    hrtimer_reprogram();
}

/* Choose LAPIC deadline = min(next-hrtimer, next-periodic-tick).
 * Fast-Path: kein hrtimer pending -> periodischer Tick.
 * Hot path called from hrtimer_start/cancel und timer_handler tail. */
void hrtimer_reprogram(void) {
    if (__builtin_expect(!tickless_active, 0)) return;

    uint64_t now = hrtimer_now_ns();
    uint64_t deadline_ns = next_periodic_tick_ns;

    uint64_t flags;
    spin_lock_irq(&base.lock, &flags);
    rb_node_t *leftmost = base.tree.leftmost;
    if (leftmost) {
        hrtimer_t *t = rb_entry(leftmost, hrtimer_t, node);
        if (t->deadline_ns < deadline_ns)
            deadline_ns = t->deadline_ns;
    }
    spin_unlock_irq(&base.lock, flags);

    /* Linus-today: lieber 1us zu frueh feuern als zu spaet. */
    uint64_t delta_ns = (deadline_ns > now) ? (deadline_ns - now) : 1000;
    lapic_arm_ns(delta_ns);
}
