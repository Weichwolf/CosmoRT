/* CosmoRT IPv6 Routing — per-NS prefix table.
 *
 * Linux uses an FIB-Tree (rt_tree). We keep this simple: linear list of
 * (prefix, prefix_len, gateway, oif), longest-prefix-match on lookup.
 * Sufficient for the dual-NIC + loopback scenarios we support; the
 * datapath is anyway dominated by neighbour resolution, not route walk. */
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
