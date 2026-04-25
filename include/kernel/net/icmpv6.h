/* CosmoRT ICMPv6 — RFC 4443 + 4861 (NDP).
 *
 * Echo Request/Reply, Destination Unreachable, plus the four NDP message
 * types (NS/NA/RS/RA) which we route through ndp.c. */
#ifndef COSMO_NET_ICMPV6_H
#define COSMO_NET_ICMPV6_H

#include <stdint.h>
#include "net/in6.h"
#include "net/ipv6.h"

/* ICMPv6 header (4 bytes type+code+cksum). */
typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t cksum;
} icmpv6_hdr_t;

/* RFC 4443 §2.4: pseudo-header checksum across IPv6 src+dst+plen+nexthdr. */
uint16_t icmpv6_cksum(const struct in6_addr *src,
                      const struct in6_addr *dst,
                      const uint8_t *icmp_payload, int icmp_len);

/* Hand a fully-parsed v6 packet to ICMPv6. p->next_hdr == 58. */
void icmpv6_input(uint32_t ns_id, const ipv6_pkt_t *p);

/* Send Echo Reply in response to Echo Request — used by icmpv6_input,
 * exposed for tests. */
void icmpv6_send_echo_reply(uint32_t ns_id, const ipv6_pkt_t *req,
                            const uint8_t *body, int body_len);

/* Construct + send a Destination Unreachable (Type 1, Code 4 = port). */
void icmpv6_send_port_unreach(uint32_t ns_id, const ipv6_pkt_t *orig);

/* ICMPv6 Code values for Type=1 Destination Unreachable */
#define ICMPV6_DU_NOROUTE   0
#define ICMPV6_DU_ADM       1
#define ICMPV6_DU_BEYONDSC  2
#define ICMPV6_DU_ADDR      3
#define ICMPV6_DU_PORT      4

#endif /* COSMO_NET_ICMPV6_H */
