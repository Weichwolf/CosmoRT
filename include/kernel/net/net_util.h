/* CosmoRT Network Utilities — shared helpers for net.c, tcp.c, etc. */
#ifndef NET_UTIL_H
#define NET_UTIL_H

#include <stdint.h>
#include "arch/arch.h"

static inline void mcpy(void *d, const void *s, int n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}

static inline void mzero(void *d, int n) {
    uint8_t *dd = d; while (n--) *dd++ = 0;
}

static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8; p[1] = v;
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static inline uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t ip_cksum(const uint8_t *d, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        uint16_t w = (uint16_t)(d[i] << 8);
        if (i + 1 < len) w |= d[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static inline void net_idle(void) {
    arch_halt();
}

static inline int net_random(void *buf, int len) {
    extern int random_get(void *, unsigned long);
    return random_get(buf, (unsigned long)len);
}

#endif
