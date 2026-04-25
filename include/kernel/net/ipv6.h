/* CosmoRT IPv6 — header parser, send path. RFC 8200.
 *
 * Wire format (40 byte fixed header):
 *
 *   |  Version(4) | Traffic Class(8) |       Flow Label(20)        |
 *   |        Payload Length(16)      | Next Hdr(8) | Hop Limit(8)  |
 *   |                       Source Address(128)                    |
 *   |                    Destination Address(128)                  |
 *
 * Extension headers chain via Next Header until a transport protocol is
 * reached. We parse Hop-by-Hop (0) and Fragment (44) — Routing/DstOpts
 * are accepted but their content is skipped over by length.
 */
#ifndef COSMO_NET_IPV6_H
#define COSMO_NET_IPV6_H

#include <stdint.h>
#include "net/in6.h"

#define IPV6_HDR_LEN        40
#define IPV6_DEFAULT_HOPLIM 64

/* Parsed view of an IPv6 packet (after extension headers walked). */
typedef struct {
    const uint8_t  *frame;       /* whole ethernet frame */
    int             frame_len;
    struct in6_addr src;
    struct in6_addr dst;
    uint8_t         next_hdr;    /* final upper-layer protocol */
    uint8_t         hop_limit;
    uint16_t        payload_len; /* from fixed header */
    int             l4_off;      /* offset (into frame) of upper-layer header */
    int             l4_len;      /* bytes available at l4_off */
    uint32_t        flow_label;  /* lower 20 bits */
    uint8_t         tclass;
} ipv6_pkt_t;

/* Parse the IPv6 header + extension chain. Returns 0 on success, <0 on
 * malformed/unknown ext hdr. eth_off is where the IPv6 header starts in
 * frame (14 for plain Ethernet). */
int ipv6_parse(const uint8_t *frame, int len, int eth_off, ipv6_pkt_t *out);

/* Build IPv6 + Ethernet header in pkt[0..eth_off+IPV6_HDR_LEN). plen is
 * payload length following the IPv6 header (excludes ext headers in this
 * minimal builder — we don't emit them ourselves). */
void ipv6_build_header(uint8_t *pkt,
                       const uint8_t *dst_mac,
                       const struct in6_addr *src,
                       const struct in6_addr *dst,
                       uint8_t next_hdr, uint16_t plen, uint8_t hop_limit);

/* Send a raw IPv6 packet (full ethernet frame). Hands off to netif_tx. */
void ipv6_send_frame(const uint8_t *frame, uint16_t len);

/* IPv6 packet entry point — called from net/dispatch.c when ETHERTYPE
 * is 0x86DD. Demuxes upper-layer (TCP6, UDP6, ICMPv6) per ns_id. */
void ipv6_input(uint32_t ns_id, const uint8_t *frame, int len);

/* Source-address selection (RFC 6724, simplified): for a given destination
 * picks the best on-NS source. Loopback dst → loopback src; link-local dst
 * → link-local src; otherwise default global. Returns 0 on success. */
int ipv6_select_src(uint32_t ns_id, const struct in6_addr *dst,
                    struct in6_addr *out_src);

/* Per-NS list of configured IPv6 addresses on the loopback / default
 * netif. Walked by bind() / source-address-selection. */
typedef struct ipv6_addr {
    struct in6_addr  addr;
    uint8_t          prefix_len;     /* /64, /128 ... */
    uint8_t          dad_state;      /* see below */
    uint8_t          is_loopback;
    uint8_t          _pad;
    struct ipv6_addr *next;
} ipv6_addr_t;

#define IPV6_DAD_TENTATIVE  0
#define IPV6_DAD_OPTIMISTIC 1
#define IPV6_DAD_PREFERRED  2
#define IPV6_DAD_DEPRECATED 3

/* Returns 1 if the given address is configured locally (anywhere on this
 * NS) — used by bind() to validate sin6_addr. ::, ::1 and v4-mapped are
 * accepted by special-case. */
int ipv6_addr_local(uint32_t ns_id, const struct in6_addr *a);

/* Initialise per-NS IPv6 state — call once during net init to attach
 * ::1/128 to the boot loopback. */
void ipv6_init(void);

#endif /* COSMO_NET_IPV6_H */
