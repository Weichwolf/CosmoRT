/* CosmoRT Neighbor Discovery (RFC 4861) — IPv6 Layer-3↔Layer-2 mapping.
 *
 * Per-NS neighbour cache replaces ARP for IPv6. NS (Neighbor Solicitation,
 * Type 135) probes a target; NA (Neighbor Advertisement, Type 136) carries
 * the resolved link-layer address back. RS/RA (133/134) are the host-side
 * router-discovery messages; we receive RAs to drive SLAAC.
 *
 * NUD (Neighbor Unreachability Detection) state machine — Linux uses
 * INCOMPLETE → REACHABLE → STALE → DELAY → PROBE → FAILED. We support the
 * subset needed for loopback + simple LAN: INCOMPLETE / REACHABLE / FAILED.
 */
#ifndef COSMO_NET_NDP_H
#define COSMO_NET_NDP_H

#include <stdint.h>
#include "net/in6.h"
#include "net/ipv6.h"

enum nud6_state {
    NUD6_NONE = 0,
    NUD6_INCOMPLETE,
    NUD6_REACHABLE,
    NUD6_STALE,
    NUD6_FAILED
};

typedef struct ndp_entry {
    struct in6_addr  addr;
    uint8_t          mac[6];
    uint8_t          state;        /* enum nud6_state */
    uint8_t          _pad;
    uint32_t         ns_id;        /* owning network namespace */
    uint64_t         last_used_ms;
    struct ndp_entry *next;        /* hash chain */
} ndp_entry_t;

void ndp_init(void);

/* Lookup REACHABLE-only entry; returns 0+mac on hit, -1 on miss. */
int  ndp_lookup(uint32_t ns_id, const struct in6_addr *addr, uint8_t *mac_out);

/* Insert/refresh a (addr, mac) mapping in REACHABLE state. */
void ndp_remember(uint32_t ns_id, const struct in6_addr *addr, const uint8_t *mac);

/* Resolve via NS — returns 0/mac if cached, -EAGAIN if NS sent + caller
 * should retry, or -EHOSTUNREACH if max retries exhausted. */
int  ndp_resolve(uint32_t ns_id, const struct in6_addr *addr, uint8_t *mac_out);

/* RFC 4861 message handlers — called from icmpv6_input. */
void ndp_ns_input(uint32_t ns_id, const ipv6_pkt_t *p);
void ndp_na_input(uint32_t ns_id, const ipv6_pkt_t *p);
void ndp_rs_input(uint32_t ns_id, const ipv6_pkt_t *p);
void ndp_ra_input(uint32_t ns_id, const ipv6_pkt_t *p);

/* DAD: send a Neighbor Solicitation for `addr` from :: with the
 * solicited-node multicast destination. Used at interface bring-up. */
void ndp_send_dad_ns(uint32_t ns_id, const struct in6_addr *target);

/* Generate a link-local address fe80::<EUI-64> for the given MAC. */
void ndp_make_linklocal(const uint8_t mac[6], struct in6_addr *out);

/* Test self-checks (used by ktest unit). */
int  ipv6_self_test_linklocal(void);
int  ipv6_self_test_neigh(void);
int  ipv6_self_test_dad(void);
int  ipv6_self_test_slaac(void);

#endif
