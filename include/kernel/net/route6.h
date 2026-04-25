/* CosmoRT IPv6 Routing — per-NS prefix table.
 *
 * Linux uses an FIB-trie (radix). We keep this simple: slab-allocated
 * linked list of (prefix, prefix_len, gateway, oif), sorted by descending
 * prefix_len so first match wins LPM. No fixed cap — list and slab grow
 * with route count, so 1000+ NICs/routes work correctly.
 *
 * Performance: O(N) lookup. Acceptable up to ~100 routes; beyond that the
 * lookup dominates the datapath. Migration to fib_trie or a hash on
 * prefix_len buckets is a future optimization, not a correctness fix. */
#ifndef COSMO_NET_ROUTE6_H
#define COSMO_NET_ROUTE6_H

#include <stdint.h>
#include "net/in6.h"

struct net_ns;
struct netif;

typedef struct route6_entry {
    struct in6_addr  prefix;
    uint8_t          prefix_len;
    uint8_t          metric;
    uint8_t          _pad[2];
    struct in6_addr  gateway;       /* :: for on-link */
    struct netif    *oif;
    struct route6_entry *next;
} route6_entry_t;

void route6_init(void);

/* Add a route to the given NS. Returns 0 on success, <0 on OOM. */
int  route6_add(struct net_ns *ns, const struct in6_addr *prefix,
                uint8_t prefix_len, const struct in6_addr *gateway,
                struct netif *oif);

/* Lookup destination in NS — returns matching route or NULL. */
route6_entry_t *route6_lookup(struct net_ns *ns, const struct in6_addr *dst);

/* Self-test: ::1 must always resolve to the loopback. */
int ipv6_self_test_route(void);

#endif
