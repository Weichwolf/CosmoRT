/* CosmoRT RT/Compute Communication Primitives */
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stddef.h>
#include "arch/arch.h"

#define RT_CORE_COUNT 1

int  rt_core_id(int index);
int  rt_is_current_rt(void);

void rt_wake(int core_id);

struct thread;
void sched_wake(struct thread *t);

typedef struct {
    uint8_t *buf;
    uint32_t size;
    volatile uint32_t head;
    volatile uint32_t tail;
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

static inline int rt_channel_push(rt_channel_t *ch, const void *data, uint32_t len) {
    uint32_t needed = 4 + len;
    uint32_t h = ch->head;
    uint32_t t = arch_load_acquire(&ch->tail);
    if (h - t + needed > ch->size) return -1;

    uint32_t mask = ch->size - 1;
    const uint8_t *src = (const uint8_t *)data;

    ch->buf[(h + 0) & mask] = (uint8_t)(len);
    ch->buf[(h + 1) & mask] = (uint8_t)(len >> 8);
    ch->buf[(h + 2) & mask] = (uint8_t)(len >> 16);
    ch->buf[(h + 3) & mask] = (uint8_t)(len >> 24);

    for (uint32_t i = 0; i < len; i++)
        ch->buf[(h + 4 + i) & mask] = src[i];

    arch_store_release(&ch->head, h + needed);
    return 0;
}

static inline int rt_channel_pop(rt_channel_t *ch, void *buf, uint32_t bufsize) {
    uint32_t t = ch->tail;
    uint32_t h = arch_load_acquire(&ch->head);
    if (h == t) return -1;

    uint32_t mask = ch->size - 1;

    uint32_t len = (uint32_t)ch->buf[(t + 0) & mask]
                 | ((uint32_t)ch->buf[(t + 1) & mask] << 8)
                 | ((uint32_t)ch->buf[(t + 2) & mask] << 16)
                 | ((uint32_t)ch->buf[(t + 3) & mask] << 24);

    if (len > bufsize) return -1;
    if (h - t < 4 + len) return -1;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++)
        dst[i] = ch->buf[(t + 4 + i) & mask];

    arch_store_release(&ch->tail, t + 4 + len);
    return (int)len;
}

#endif
