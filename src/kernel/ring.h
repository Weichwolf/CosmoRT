/* Kernel-compatible SPSC ring buffer — derived from CosmoLib cl_ring.h
 *
 * Lock-free single-producer single-consumer. Lives on shared memory
 * for zero-copy IPC between kernel and userspace driver processes.
 *
 * This is a freestanding version: no mmap, no libc memcpy.
 * Uses GCC __atomic builtins instead of C11 <stdatomic.h> (freestanding).
 * Uses kmemcpy from memops.h instead of libc memcpy.
 *
 * API matches CosmoLib cl_ring exactly — same struct layout, same
 * semantics. Kernel and userspace see the same ring in shared memory.
 */
#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>
#include "memops.h"

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t mask;
    uint32_t capacity;
    int      owns_mem;
    uint8_t  data[];
} ring_t;

static inline uint32_t ring_p2(uint32_t v) {
    v--; v |= v>>1; v |= v>>2; v |= v>>4; v |= v>>8; v |= v>>16;
    return v + 1;
}

static inline size_t ring_sizeof(uint32_t capacity) {
    return sizeof(ring_t) + capacity;
}

/* Construct on pre-allocated memory (shared page, DMA buffer, etc.) */
static inline ring_t *ring_on(void *mem, size_t mem_size) {
    if (!mem || mem_size < sizeof(ring_t) + 16) return 0;
    size_t data_space = mem_size - sizeof(ring_t);
    uint32_t cap = ring_p2((uint32_t)data_space);
    if (cap > data_space) cap >>= 1;
    if (cap < 16) return 0;
    ring_t *r = (ring_t *)mem;
    __atomic_store_n(&r->head, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&r->tail, 0, __ATOMIC_SEQ_CST);
    r->mask = cap - 1;
    r->capacity = cap;
    r->owns_mem = 0;
    return r;
}

static inline size_t ring_available(const ring_t *r) {
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_SEQ_CST);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_SEQ_CST);
    return (size_t)((t - h) & r->mask);
}

static inline size_t ring_free(const ring_t *r) {
    return (size_t)(r->capacity - 1) - ring_available(r);
}

static inline size_t ring_write(ring_t *r, const void *data, size_t len) {
    if (!r || !data || len == 0) return 0;
    size_t avail = ring_free(r);
    if (len > avail) len = avail;
    if (len == 0) return 0;

    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_SEQ_CST);
    uint32_t pos = t & r->mask;

    uint32_t first = r->capacity - pos;
    if (first > len) first = (uint32_t)len;
    kmemcpy(r->data + pos, data, first);
    if (first < len)
        kmemcpy(r->data, (const uint8_t *)data + first, len - first);

    __atomic_store_n(&r->tail, t + (uint32_t)len, __ATOMIC_SEQ_CST);
    return len;
}

static inline size_t ring_read(ring_t *r, void *buf, size_t cap) {
    if (!r || !buf || cap == 0) return 0;
    size_t avail = ring_available(r);
    if (cap > avail) cap = avail;
    if (cap == 0) return 0;

    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_SEQ_CST);
    uint32_t pos = h & r->mask;

    uint32_t first = r->capacity - pos;
    if (first > cap) first = (uint32_t)cap;
    kmemcpy(buf, r->data + pos, first);
    if (first < cap)
        kmemcpy((uint8_t *)buf + first, r->data, cap - first);

    __atomic_store_n(&r->head, h + (uint32_t)cap, __ATOMIC_SEQ_CST);
    return cap;
}

#endif
