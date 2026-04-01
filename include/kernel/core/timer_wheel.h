/* CosmoRT Timer Wheel — single-core, 1ms granularity, 256 slots.
 *
 * Cascading: timers > 256ms use remaining_rounds counter.
 * Each tick advances current_slot. When a timer fires, remaining_rounds
 * is checked — if > 0, decrement and skip. If 0, fire callback.
 */
#ifndef TIMER_WHEEL_H
#define TIMER_WHEEL_H

#include <stdint.h>

/* ── Timer Wheel Config ──────────────────────────── */

#define TW_SLOTS      256       /* 1ms per slot → 256ms full revolution */
#define TW_MAX_TIMERS 256       /* max concurrent active timers */

/* ── Timer Actions ───────────────────────────────── */

enum tw_action {
    TW_ACTION_NONE = 0,
    RT_TIMER_TCP_RETRANSMIT,
    RT_TIMER_TCP_KEEPALIVE,
    RT_TIMER_CANCEL
};

/* ── Timer Entry (static pool) ───────────────────── */

typedef struct {
    void    *ctx;
    uint32_t expiry_tick;
    uint16_t remaining_rounds;
    uint8_t  action;
    uint8_t  active;
    int      slot;
    int      next;              /* intrusive slist: -1 = end */
} tw_entry_t;

/* ── Timer Wheel ─────────────────────────────────── */

typedef struct {
    int       slots[TW_SLOTS];
    tw_entry_t entries[TW_MAX_TIMERS];
    uint64_t  current_tick;
    int       current_slot;
} timer_wheel_t;

/* ── API ─────────────────────────────────────────── */

void timer_wheel_init(void);
void timer_wheel_tick(void);
int  timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms);
int  timer_wheel_cancel(void *ctx);

uint64_t timer_wheel_current_tick(void);
int      timer_wheel_active_count(void);

#endif
