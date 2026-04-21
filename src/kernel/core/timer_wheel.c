#include "core/timer_wheel.h"
#include "core/timer.h"
#include "hw/serial.h"
#include "mm/slab.h"
#include "spinlock.h"

static timer_wheel_t tw;
static uint64_t      tw_last_ms;

static slab_t     tw_slab;
static int        tw_slab_inited;
static spinlock_t tw_lock = SPINLOCK_INIT;

static void tw_slab_ensure(void) {
    if (__builtin_expect(tw_slab_inited, 1)) return;
    if (__sync_bool_compare_and_swap(&tw_slab_inited, 0, 1))
        slab_init_dynamic(&tw_slab, (int)sizeof(tw_entry_t), 0);
}

__attribute__((cold))
void timer_wheel_init(void) {
    for (int i = 0; i < TW_SLOTS; i++)
        tw.slots[i] = 0;
    tw.current_tick  = 0;
    tw.current_slot  = 0;
    tw.active_count  = 0;
    tw_last_ms       = timer_ms();
    tw_slab_ensure();
}

int timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms) {
    tw_slab_ensure();
    if (timeout_ms == 0) timeout_ms = 1;

    tw_entry_t *e = (tw_entry_t *)slab_alloc(&tw_slab);
    if (__builtin_expect(!e, 0)) return -1;

    e->action = action;
    e->ctx    = ctx;

    uint32_t ticks = timeout_ms;
    uint16_t rounds = (uint16_t)((ticks > TW_SLOTS) ? (ticks / TW_SLOTS - 1) : 0);
    uint32_t slot_offset = (ticks <= TW_SLOTS) ? ticks : (ticks % TW_SLOTS);
    if (slot_offset == 0) slot_offset = TW_SLOTS;

    uint64_t flags;
    spin_lock_irq(&tw_lock, &flags);
    int target_slot = (tw.current_slot + (int)slot_offset) % TW_SLOTS;
    e->remaining_rounds = rounds;
    e->expiry_tick      = (uint32_t)(tw.current_tick + ticks);
    e->slot             = (int16_t)target_slot;
    e->next             = tw.slots[target_slot];
    tw.slots[target_slot] = e;
    tw.active_count++;
    spin_unlock_irq(&tw_lock, flags);

    return 0;
}

int timer_wheel_cancel(void *ctx) {
    int cancelled = 0;
    uint64_t flags;
    spin_lock_irq(&tw_lock, &flags);
    for (int s = 0; s < TW_SLOTS; s++) {
        tw_entry_t **pp = &tw.slots[s];
        while (*pp) {
            tw_entry_t *e = *pp;
            if (e->ctx == ctx) {
                *pp = e->next;
                tw.active_count--;
                spin_unlock_irq(&tw_lock, flags);
                slab_free(&tw_slab, e);
                cancelled++;
                spin_lock_irq(&tw_lock, &flags);
            } else {
                pp = &e->next;
            }
        }
    }
    spin_unlock_irq(&tw_lock, flags);
    return cancelled;
}

static void tw_fire(tw_entry_t *e) {
    if (!e->ctx) return;
    switch (e->action) {
    case RT_TIMER_TCP_RETRANSMIT: {
        extern void net_tcp_retransmit(void *conn);
        net_tcp_retransmit(e->ctx);
        break;
    }
    case RT_TIMER_TCP_KEEPALIVE: {
        extern void net_tcp_keepalive_probe(void *conn);
        net_tcp_keepalive_probe(e->ctx);
        break;
    }
    default:
        break;
    }
}

static void tw_process_slot(int slot) {
    uint64_t flags;
    spin_lock_irq(&tw_lock, &flags);

    tw_entry_t *head = tw.slots[slot];
    tw_entry_t *keep = 0;

    while (head) {
        tw_entry_t *e = head;
        head = head->next;

        if (e->remaining_rounds > 0) {
            e->remaining_rounds--;
            e->next = keep;
            keep = e;
        } else {
            tw.active_count--;
            e->slot = -1;
            spin_unlock_irq(&tw_lock, flags);
            tw_fire(e);
            slab_free(&tw_slab, e);
            spin_lock_irq(&tw_lock, &flags);
        }
    }

    tw.slots[slot] = keep;
    spin_unlock_irq(&tw_lock, flags);
}

void timer_wheel_tick(void) {
    uint64_t now = timer_ms();
    if (tw_last_ms == 0) { tw_last_ms = now; return; }

    uint64_t elapsed = now - tw_last_ms;
    if (elapsed == 0) return;
    if (elapsed > TW_SLOTS * 2) elapsed = TW_SLOTS * 2;

    for (uint64_t i = 0; i < elapsed; i++) {
        tw.current_tick++;
        tw.current_slot = (int)(tw.current_tick % TW_SLOTS);
        tw_process_slot(tw.current_slot);
    }

    tw_last_ms = now;
}

uint64_t timer_wheel_current_tick(void) { return tw.current_tick; }

int timer_wheel_active_count(void) { return tw.active_count; }
