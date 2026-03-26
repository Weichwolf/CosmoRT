/* CosmoRT Timer Wheel — RT-Core owned, no locks.
 *
 * 256 slots, 1ms tick. Timers > 256ms use remaining_rounds.
 * Compute-Cores post requests via timer_req_ring (SPSC rt_channel).
 * RT-Core drains requests + advances wheel in timer IRQ handler.
 */

#include "core/timer_wheel.h"
#include "core/timer.h"
#include "core/rt_poll.h"
#include "hw/serial.h"

/* ── Global timer wheel instance ─────────────────── */

static timer_wheel_t tw;
static uint64_t tw_last_ms;

/* ── Free-Stack: O(1) alloc/free ────────────────── */

static int free_stack[TW_MAX_TIMERS];
static int free_top;  /* points to next free slot (stack grows upward) */

/* ── Timer Request Ring (Compute→RT) ─────────────── */

#define TW_REQ_RING_SIZE 1024   /* power of 2 */
static uint8_t tw_req_buf[TW_REQ_RING_SIZE] __attribute__((aligned(64)));
static rt_channel_t tw_req_ring;

/* ── Init ────────────────────────────────────────── */

__attribute__((cold))
void timer_wheel_init(void) {
    for (int i = 0; i < TW_SLOTS; i++)
        tw.slots[i] = -1;

    for (int i = 0; i < TW_MAX_TIMERS; i++) {
        tw.entries[i].active = 0;
        tw.entries[i].next = -1;
        tw.entries[i].slot = -1;
    }

    /* Fill free-stack: all indices available, pop from top */
    for (int i = 0; i < TW_MAX_TIMERS; i++)
        free_stack[i] = i;
    free_top = TW_MAX_TIMERS;

    tw.current_tick = 0;
    tw.current_slot = 0;
    tw_last_ms = timer_ms();

    rt_channel_init(&tw_req_ring, tw_req_buf, TW_REQ_RING_SIZE);

    extern int timer_poll(int max_work);
    rt_poll_register(RT_PRIO_TIMER, timer_poll, 0);
}

/* ── Pool alloc/free ─────────────────────────────── */

static int tw_alloc_entry(void) {
    if (__builtin_expect(free_top == 0, 0))
        return -1;
    int idx = free_stack[--free_top];
    return idx;
}

static void tw_free_entry(int idx) {
    tw.entries[idx].active = 0;
    tw.entries[idx].next = -1;
    tw.entries[idx].slot = -1;
    free_stack[free_top++] = idx;
}

/* ── Remove entry from its slot list ─────────────── */

static void tw_unlink(int idx) {
    int slot = tw.entries[idx].slot;
    if (slot < 0) return;

    int *pp = &tw.slots[slot];
    while (*pp != -1) {
        if (*pp == idx) {
            *pp = tw.entries[idx].next;
            tw.entries[idx].next = -1;
            tw.entries[idx].slot = -1;
            return;
        }
        pp = &tw.entries[*pp].next;
    }
}

/* ── Add timer ───────────────────────────────────── */

int timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = 1;

    int idx = tw_alloc_entry();
    if (idx < 0) return -1;

    tw_entry_t *e = &tw.entries[idx];
    e->action = action;
    e->ctx = ctx;
    e->active = 1;

    uint32_t ticks = timeout_ms;
    e->remaining_rounds = (uint16_t)((ticks > TW_SLOTS) ? (ticks / TW_SLOTS - 1) : 0);
    uint32_t slot_offset = (ticks <= TW_SLOTS) ? ticks : (ticks % TW_SLOTS);
    if (slot_offset == 0) slot_offset = TW_SLOTS; /* full revolution, not slot 0 */
    int target_slot = (tw.current_slot + (int)slot_offset) % TW_SLOTS;

    e->expiry_tick = (uint32_t)(tw.current_tick + ticks);
    e->slot = target_slot;

    /* Prepend to slot list */
    e->next = tw.slots[target_slot];
    tw.slots[target_slot] = idx;

    return idx;
}

/* ── Cancel all timers for context ───────────────── */

int timer_wheel_cancel(void *ctx) {
    int cancelled = 0;
    for (int i = 0; i < TW_MAX_TIMERS; i++) {
        if (tw.entries[i].active && tw.entries[i].ctx == ctx) {
            tw_unlink(i);
            tw_free_entry(i);
            cancelled++;
        }
    }
    return cancelled;
}

/* ── Fire callback ───────────────────────────────── */

static void tw_fire(tw_entry_t *e) {
    switch (e->action) {
    case RT_TIMER_TCP_RETRANSMIT: {
        /* net_tcp_retransmit will be called when TCP retransmit is implemented.
         * For now, the context (tcp_conn pointer) is available for future use. */
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

/* ── Tick: advance wheel, fire expired ───────────── */

/* Process a single slot: fire expired, keep cascading entries */
static void tw_process_slot(int slot) {
    int idx = tw.slots[slot];
    int keep = -1;

    while (idx != -1) {
        int next = tw.entries[idx].next;
        tw_entry_t *e = &tw.entries[idx];

        if (e->remaining_rounds > 0) {
            e->remaining_rounds--;
            e->next = keep;
            keep = idx;
        } else {
            tw_fire(e);
            tw_free_entry(idx);
        }
        idx = next;
    }

    tw.slots[slot] = keep;
}

void timer_wheel_tick(void) {
    if (!rt_is_current_rt()) return;
    uint64_t now = timer_ms();
    if (tw_last_ms == 0) { tw_last_ms = now; return; }

    /* Catch up: process all elapsed milliseconds since last call.
     * LAPIC timer fires every ~66ms, but wheel granularity is 1ms.
     * Cap at 2 full revolutions to avoid stalling on huge gaps. */
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

/* ── Drain timer requests from Compute-Cores ─────── */

void timer_wheel_drain_requests(void) {
    if (!rt_is_current_rt()) return;
    tw_request_t req;
    while (rt_channel_pop(&tw_req_ring, &req, sizeof(req)) == (int)sizeof(req)) {
        if (req.action == RT_TIMER_CANCEL) {
            timer_wheel_cancel(req.ctx);
        } else {
            timer_wheel_add(req.action, req.ctx, req.timeout_ms);
        }
    }
}

/* ── Compute-Core: post timer request ────────────── */

int rt_timer_request(uint8_t action, void *ctx, uint32_t timeout_ms) {
    tw_request_t req = {
        .ctx = ctx,
        .timeout_ms = timeout_ms,
        .action = action,
        ._pad = {0, 0, 0}
    };
    return rt_channel_push(&tw_req_ring, &req, sizeof(req));
}

/* ── Stats ───────────────────────────────────────── */

uint64_t timer_wheel_current_tick(void) { return tw.current_tick; }

int timer_wheel_active_count(void) {
    return TW_MAX_TIMERS - free_top;
}

/* ── rt_poll handler ─────────────────────────────── */

int timer_poll(int max_work) {
    (void)max_work;
    timer_wheel_drain_requests();
    timer_wheel_tick();
    return 0; /* timers are fire-and-forget, no "more work" signal */
}
