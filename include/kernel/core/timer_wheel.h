/* CosmoRT Timer Wheel — RT-Core owned, 1ms granularity, 256 slots.
 *
 * Single-threaded on RT-Core (Core 0). No locks needed.
 * Compute-Cores post requests via rt_channel (timer_request_ring).
 *
 * Cascading: timers > 256ms use remaining_rounds counter.
 * Each tick advances current_slot. When a timer fires, remaining_rounds
 * is checked — if > 0, decrement and skip. If 0, fire callback.
 */
#ifndef TIMER_WHEEL_H
#define TIMER_WHEEL_H

#include <stdint.h>
#include "core/rt.h"

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
    void    *ctx;               /* opaque context (e.g. tcp_conn pointer) */
    uint32_t expiry_tick;       /* absolute tick when timer fires */
    uint16_t remaining_rounds;  /* cascading: rounds left before fire */
    uint8_t  action;            /* enum tw_action */
    uint8_t  active;            /* 1 = in use, 0 = free */
    int      slot;              /* which slot list this entry lives in */
    int      next;              /* intrusive slist: index of next entry, -1 = end */
} tw_entry_t;

/* ── Timer Wheel ─────────────────────────────────── */

typedef struct {
    int       slots[TW_SLOTS];          /* head index per slot (-1 = empty) */
    tw_entry_t entries[TW_MAX_TIMERS];  /* static pool */
    uint64_t  current_tick;             /* monotonic tick counter (ms) */
    int       current_slot;             /* slots index = current_tick % TW_SLOTS */
} timer_wheel_t;

/* ── Timer Request (Compute→RT via rt_channel) ──── */

typedef struct {
    void    *ctx;
    uint32_t timeout_ms;
    uint8_t  action;            /* enum tw_action */
    uint8_t  _pad[3];
} tw_request_t;

/* ── API (all called on RT-Core only) ────────────── */

/* Initialize timer wheel. Call once at boot. */
void timer_wheel_init(void);

/* Advance one tick (1ms). Fires expired timers. */
void timer_wheel_tick(void);

/* Drain pending timer requests from Compute-Cores. */
void timer_wheel_drain_requests(void);

/* Add a timer. Returns entry index or -1 if pool exhausted. */
int  timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms);

/* Cancel all timers for a given context. Returns number cancelled. */
int  timer_wheel_cancel(void *ctx);

/* ── Compute-Core API ────────────────────────────── */

/* Post a timer request from Compute-Core to RT-Core via rt_channel.
 * Non-blocking. Returns 0 on success, -1 if ring full. */
int  rt_timer_request(uint8_t action, void *ctx, uint32_t timeout_ms);

/* Statistics (for testing) */
uint64_t timer_wheel_current_tick(void);
int      timer_wheel_active_count(void);

#endif
