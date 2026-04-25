/* CosmoRT IPv6 — kernel-side helpers for struct in6_addr / sockaddr_in6.
 *
 * The wire/UAPI definitions live in <linux/in6.h>. This header adds
 * convenience inline accessors and the well-known address constants
 * exposed as immutable struct objects (Linux kernel calls them
 * in6addr_any / in6addr_loopback). */
#ifndef COSMO_KERNEL_IN6_H
#define COSMO_KERNEL_IN6_H

#include "linux/in6.h"

extern const struct in6_addr in6addr_any;        /* :: */
extern const struct in6_addr in6addr_loopback;   /* ::1 */

/* Compare two in6_addrs, return 1 on match. */
static inline int in6_eq(const struct in6_addr *a, const struct in6_addr *b) {
    for (int i = 0; i < 16; i++)
        if (a->s6_addr[i] != b->s6_addr[i]) return 0;
    return 1;
}

static inline int in6_is_loopback(const struct in6_addr *a) {
    return in6_eq(a, &in6addr_loopback);
}

static inline int in6_is_any(const struct in6_addr *a) {
    return in6_eq(a, &in6addr_any);
}

/* IPv4-mapped IPv6 address: ::ffff:a.b.c.d (RFC 4291 §2.5.5.2).
 * First 80 bits zero, next 16 bits 0xFFFF, last 32 bits IPv4. */
static inline int in6_is_v4mapped(const struct in6_addr *a) {
    for (int i = 0; i < 10; i++) if (a->s6_addr[i] != 0) return 0;
    return a->s6_addr[10] == 0xFF && a->s6_addr[11] == 0xFF;
}

/* Link-local prefix fe80::/10 (RFC 4291 §2.5.6). */
static inline int in6_is_linklocal(const struct in6_addr *a) {
    return a->s6_addr[0] == 0xFE && (a->s6_addr[1] & 0xC0) == 0x80;
}

/* Solicited-node multicast prefix ff02::1:ff00:0/104 (RFC 4291 §2.7.1). */
static inline int in6_is_solicited_node(const struct in6_addr *a) {
    return a->s6_addr[0] == 0xFF && a->s6_addr[1] == 0x02 &&
           a->s6_addr[11] == 0x01 && a->s6_addr[12] == 0xFF;
}

static inline void in6_copy(struct in6_addr *d, const struct in6_addr *s) {
    for (int i = 0; i < 16; i++) d->s6_addr[i] = s->s6_addr[i];
}

#endif /* COSMO_KERNEL_IN6_H */
