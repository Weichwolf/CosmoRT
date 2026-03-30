/* CosmoRT Timer Wheel — RT-Core owned, 1ms granularity, 256 slots. */
#ifndef TIMER_WHEEL_H
#define TIMER_WHEEL_H

#include <stdint.h>
#include "core/rt.h"

#define TW_SLOTS      256
#define TW_MAX_TIMERS 256

enum tw_action {
    TW_ACTION_NONE = 0,
    RT_TIMER_TCP_RETRANSMIT,
    RT_TIMER_TCP_KEEPALIVE,
    RT_TIMER_CANCEL
};

typedef struct {
    void    *ctx;
    uint32_t expiry_tick;
    uint16_t remaining_rounds;
    uint8_t  action;
    uint8_t  active;
    int      slot;
    int      next;
} tw_entry_t;

typedef struct {
    int       slots[TW_SLOTS];
    tw_entry_t entries[TW_MAX_TIMERS];
    uint64_t  current_tick;
    int       current_slot;
} timer_wheel_t;

typedef struct {
    void    *ctx;
    uint32_t timeout_ms;
    uint8_t  action;
    uint8_t  _pad[3];
} tw_request_t;

void timer_wheel_init(void);

void timer_wheel_tick(void);

void timer_wheel_drain_requests(void);

int  timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms);

int  timer_wheel_cancel(void *ctx);

int  rt_timer_request(uint8_t action, void *ctx, uint32_t timeout_ms);

uint64_t timer_wheel_current_tick(void);
int      timer_wheel_active_count(void);

#endif
