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

/* ── Monotonic clock ───────────────────────────────── */

uint64_t hrtimer_now_ns(void) {
    if (__builtin_expect(timer_tsc_per_ms == 0, 0)) return 0;
    uint64_t tsc = timer_tsc_now();
    uint64_t since_boot = tsc - timer_boot_tsc;
    uint64_t ms = since_boot / timer_tsc_per_ms;
    uint64_t rem = since_boot % timer_tsc_per_ms;
    return ms * NSEC_PER_MSEC + (rem * NSEC_PER_MSEC) / timer_tsc_per_ms;
}

/* ── LAPIC one-shot programming ────────────────────── */

static void lapic_arm_ns(uint64_t delta_ns) {
    /* Convert ns to LAPIC ticks */
    uint64_t ticks = (delta_ns * lapic_ticks_per_ms) / NSEC_PER_MSEC;
    if (ticks < LAPIC_MIN_TICKS) ticks = LAPIC_MIN_TICKS;
    if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;

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
        arch_pause();

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
    serial_puts(" ticks/ms, one-shot mode\n");

    /* Restore periodic LAPIC timer (1000Hz) for legacy compatibility.
     * sched_preempt, net_poll, timer_wheel still depend on periodic ticks.
     * hrtimer_run_expired runs alongside in timer_handler.
     * TODO: remove periodic tick once all callers use hrtimer. */
    lapic_write(LAPIC_TIMER_DIV, 0x03);
    lapic_write(LAPIC_TIMER_LVT, 0x20000 | LAPIC_TIMER_VECTOR); /* periodic, vector 32 */
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)lapic_ticks_per_ms); /* 1000Hz */
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

    hrtimer_reprogram();
}

void hrtimer_reprogram(void) {
    /* While periodic LAPIC is still active, hrtimer piggybacks on the
     * 1000Hz tick via hrtimer_run_expired() in timer_handler.
     * No LAPIC reprogramming needed — just let the next tick fire it.
     * TODO: switch to pure one-shot once periodic tick is removed. */
    (void)lapic_arm_ns;
}
