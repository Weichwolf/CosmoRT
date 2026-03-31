/* CosmoRT RT/Compute Communication Primitives
 *
 * RT-Core (Core 0) handles all IRQs and I/O. Compute-Cores (1..N) run userspace.
 * Communication is via lock-free SPSC channels. RT-Core never blocks.
 *
 * rt_channel_t: single-producer single-consumer ringbuffer.
 * Messages are framed: 4-byte little-endian length header + payload.
 * head/tail are monotonically increasing uint32_t (wrap at 2^32).
 * Buffer access via (index & (size-1)). size must be power of 2.
 */
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stddef.h>
#include "arch/arch.h"

/* ── RT-Core identification ──────────────────────── */

#define RT_CORE_COUNT 1

int  rt_core_id(int index);      /* physical core ID of RT-Core N (index 0 -> Core 0) */
int  rt_is_current_rt(void);     /* 1 if current core is an RT-Core */

/* ── IPI Wake ────────────────────────────────────── */

/* Send reschedule IPI to a specific core. Breaks hlt, sets need_resched.
 * Fire-and-forget, safe from IRQ context (RT-Core never blocks). */
void rt_wake(int core_id);

/* Wake a blocked/sleeping thread. Sets THREAD_RUNNABLE, enqueues in
 * target core's run queue, sends IPI if on different core.
 * IRQ-safe (can be called from IRQ handlers on RT-Core). */
struct thread;
void sched_wake(struct thread *t);

/* ── SPSC Lock-free Ringbuffer ───────────────────── */

typedef struct {
    uint8_t *buf;           /* backing buffer */
    uint32_t size;          /* buffer size (power of 2) */
    volatile uint32_t head; /* producer writes here (monotonic) */
    volatile uint32_t tail; /* consumer reads here (monotonic) */
} rt_channel_t;

static inline int rt_channel_init(rt_channel_t *ch, void *buf, uint32_t size) {
    if (size == 0 || (size & (size - 1)) != 0) return -1;
    ch->buf  = (uint8_t *)buf;
    ch->size = size;
    ch->head = 0;
    ch->tail = 0;
    return 0;
}

static inline int rt_channel_used(rt_channel_t *ch) {
    uint32_t h = arch_load_acquire(&ch->head);
    uint32_t t = arch_load_acquire(&ch->tail);
    return (int)(h - t);
}

static inline int rt_channel_free(rt_channel_t *ch) {
    return (int)ch->size - rt_channel_used(ch);
}

/* Non-blocking push. Returns 0 on success, -1 if full.
 * Message format: [4-byte len LE][payload] */
static inline int rt_channel_push(rt_channel_t *ch, const void *data, uint32_t len) {
    uint32_t needed = 4 + len;
    uint32_t h = ch->head;                          /* only producer reads head */
    uint32_t t = arch_load_acquire(&ch->tail);       /* consumer may advance tail */
    if (h - t + needed > ch->size) return -1;

    uint32_t mask = ch->size - 1;
    const uint8_t *src = (const uint8_t *)data;

    /* Length header (little-endian, byte-by-byte for wrap safety) */
    ch->buf[(h + 0) & mask] = (uint8_t)(len);
    ch->buf[(h + 1) & mask] = (uint8_t)(len >> 8);
    ch->buf[(h + 2) & mask] = (uint8_t)(len >> 16);
    ch->buf[(h + 3) & mask] = (uint8_t)(len >> 24);

    for (uint32_t i = 0; i < len; i++)
        ch->buf[(h + 4 + i) & mask] = src[i];

    arch_store_release(&ch->head, h + needed);
    return 0;
}

/* Non-blocking pop. Returns message length or -1 if empty. */
static inline int rt_channel_pop(rt_channel_t *ch, void *buf, uint32_t bufsize) {
    uint32_t t = ch->tail;                           /* only consumer reads tail */
    uint32_t h = arch_load_acquire(&ch->head);       /* producer may advance head */
    if (h == t) return -1;

    uint32_t mask = ch->size - 1;

    /* Read length header */
    uint32_t len = (uint32_t)ch->buf[(t + 0) & mask]
                 | ((uint32_t)ch->buf[(t + 1) & mask] << 8)
                 | ((uint32_t)ch->buf[(t + 2) & mask] << 16)
                 | ((uint32_t)ch->buf[(t + 3) & mask] << 24);

    if (len > bufsize) return -1;
    if (h - t < 4 + len) return -1;  /* incomplete message */

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++)
        dst[i] = ch->buf[(t + 4 + i) & mask];

    arch_store_release(&ch->tail, t + 4 + len);
    return (int)len;
}

#endif
