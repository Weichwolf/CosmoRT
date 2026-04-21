#ifndef TIMER_WHEEL_H
#define TIMER_WHEEL_H

#include <stdint.h>

#define TW_SLOTS 256

enum tw_action {
    TW_ACTION_NONE = 0,
    RT_TIMER_TCP_RETRANSMIT,
    RT_TIMER_TCP_KEEPALIVE,
    RT_TIMER_CANCEL
};

typedef struct tw_entry {
    struct tw_entry *next;
    void            *ctx;
    uint32_t         expiry_tick;
    uint16_t         remaining_rounds;
    uint8_t          action;
    int16_t          slot;
} tw_entry_t;

typedef struct {
    tw_entry_t *slots[TW_SLOTS];
    uint64_t    current_tick;
    int         current_slot;
    int         active_count;
} timer_wheel_t;

void timer_wheel_init(void);
void timer_wheel_tick(void);
int  timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms);
int  timer_wheel_cancel(void *ctx);

uint64_t timer_wheel_current_tick(void);
int      timer_wheel_active_count(void);

#endif
