/* CosmoRT ICMPv6 — Echo + Destination Unreachable + NDP routing.
 *
 * Type/Code matrix supported:
 *   1   (Dest Unreach)  TX-only — sent on UDP6 port-unreach.
 *   128 (Echo Request)  RX → emit Echo Reply (ping6).
 *   129 (Echo Reply)    RX → silently consumed (handled by raw sockets if any).
 *   133/134 (RS/RA)     forwarded to ndp_rs_input / ndp_ra_input.
 *   135/136 (NS/NA)     forwarded to ndp_ns_input / ndp_na_input.
 */

#include "net/icmpv6.h"
#include "net/in6.h"
#include "net/ipv6.h"
#include "net/net.h"
#include "net/net_util.h"
#include "net/netif.h"

/* Weak stubs — real implementations land with ndp.c. */
__attribute__((weak)) void ndp_ns_input(uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; }
__attribute__((weak)) void ndp_na_input(uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; }
__attribute__((weak)) void ndp_rs_input(uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; }
__attribute__((weak)) void ndp_ra_input(uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; }

/* ── Pseudo-header checksum (RFC 4443 §2.4 / RFC 8200 §8.1) ──────
 *
 *   pseudo  = src(16) | dst(16) | icmp_len(4 BE) | zeros(3) | next_hdr(1=58)
 *   actual  = ones-complement sum over pseudo + ICMP payload, with the
 *             cksum field zeroed during compute.
 */
uint16_t icmpv6_cksum(const struct in6_addr *src,
                      const struct in6_addr *dst,
                      const uint8_t *icmp_payload, int icmp_len) {
    uint32_t sum = 0;
    /* src + dst */
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)((src->s6_addr[i] << 8) | src->s6_addr[i + 1]);
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)((dst->s6_addr[i] << 8) | dst->s6_addr[i + 1]);
    /* upper-layer length (32-bit BE) — fold into 16-bit words. */
    sum += (uint32_t)((icmp_len >> 16) & 0xFFFF);
    sum += (uint32_t)(icmp_len & 0xFFFF);
    /* zero(24) | next_hdr(8=58) */
    sum += 58;
    /* payload */
    for (int i = 0; i < icmp_len; i += 2) {
        uint16_t w = (uint16_t)(icmp_payload[i] << 8);
        if (i + 1 < icmp_len) w |= icmp_payload[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Echo Reply ─────────────────────────────────── */

void icmpv6_send_echo_reply(uint32_t ns_id, const ipv6_pkt_t *req,
                            const uint8_t *body, int body_len) {
    (void)ns_id;
    /* Build Ethernet+IPv6+ICMPv6 in one shot. */
    if (body_len < 4 || body_len > 1400) return;
    uint8_t pkt[1536];
    mzero(pkt, sizeof(pkt));

    /* Reverse src↔dst; pick a sensible source. */
    struct in6_addr src;
    if (in6_is_loopback(&req->dst))      in6_copy(&src, &in6addr_loopback);
    else                                 ipv6_select_src(ns_id, &req->src, &src);

    /* Reply MAC: for loopback just zero — netif_tx routes by IP not MAC. */
    uint8_t dst_mac[6] = {0};
    /* Copy peer MAC from the request frame (offset 6 = src MAC). */
    for (int i = 0; i < 6; i++) dst_mac[i] = req->frame[6 + i];

    int icmp_len = body_len; /* type/code/cksum/id/seq + data already counted */
    ipv6_build_header(pkt, dst_mac, &src, &req->src,
                      IPPROTO_ICMPV6, (uint16_t)icmp_len,
                      IPV6_DEFAULT_HOPLIM);

    /* ICMPv6 starts at 14+40 = 54 */
    uint8_t *ic = pkt + 54;
    /* Body layout: type, code, cksum, identifier, sequence, payload */
    for (int i = 0; i < icmp_len; i++) ic[i] = body[i];
    ic[0] = ICMPV6_ECHO_REPLY;
    ic[1] = 0;
    ic[2] = 0; ic[3] = 0;
    uint16_t c = icmpv6_cksum(&src, &req->src, ic, icmp_len);
    ic[2] = (uint8_t)(c >> 8);
    ic[3] = (uint8_t)c;

    ipv6_send_frame(pkt, (uint16_t)(54 + icmp_len));
}

void icmpv6_send_port_unreach(uint32_t ns_id, const ipv6_pkt_t *orig) {
    (void)ns_id;
    /* Body = unused(4) + as much of orig as fits in MTU - headers. */
    int orig_avail = orig->frame_len - 14; /* IPv6 header + payload */
    if (orig_avail > 1232) orig_avail = 1232; /* min IPv6 MTU 1280 - 8 ICMP - 40 IP */
    int icmp_len = 8 + orig_avail;

    uint8_t pkt[1536];
    mzero(pkt, sizeof(pkt));

    struct in6_addr src;
    ipv6_select_src(ns_id, &orig->src, &src);
    uint8_t dst_mac[6] = {0};
    for (int i = 0; i < 6; i++) dst_mac[i] = orig->frame[6 + i];

    ipv6_build_header(pkt, dst_mac, &src, &orig->src,
                      IPPROTO_ICMPV6, (uint16_t)icmp_len,
                      IPV6_DEFAULT_HOPLIM);

    uint8_t *ic = pkt + 54;
    ic[0] = ICMPV6_DEST_UNREACH;
    ic[1] = ICMPV6_DU_PORT;
    ic[2] = 0; ic[3] = 0;
    /* unused */
    ic[4] = ic[5] = ic[6] = ic[7] = 0;
    /* original packet starting at IPv6 header */
    for (int i = 0; i < orig_avail; i++) ic[8 + i] = orig->frame[14 + i];

    uint16_t c = icmpv6_cksum(&src, &orig->src, ic, icmp_len);
    ic[2] = (uint8_t)(c >> 8);
    ic[3] = (uint8_t)c;

    ipv6_send_frame(pkt, (uint16_t)(54 + icmp_len));
}

/* ── Demux ──────────────────────────────────────── */

void icmpv6_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    if (p->l4_len < 4) return;
    const uint8_t *ic = p->frame + p->l4_off;
    /* Verify checksum to catch bit-rot — silently drop on mismatch. */
    uint16_t got = (uint16_t)((ic[2] << 8) | ic[3]);
    uint8_t  copy[1500];
    int n = p->l4_len > (int)sizeof(copy) ? (int)sizeof(copy) : p->l4_len;
    for (int i = 0; i < n; i++) copy[i] = ic[i];
    copy[2] = copy[3] = 0;
    uint16_t want = icmpv6_cksum(&p->src, &p->dst, copy, n);
    if (got != want) return;

    uint8_t type = ic[0];
    switch (type) {
    case ICMPV6_ECHO_REQUEST:
        icmpv6_send_echo_reply(ns_id, p, ic, p->l4_len);
        return;
    case ICMPV6_ECHO_REPLY:
        /* Currently no raw socket layer — drop. */
        return;
    case ICMPV6_NB_SOLICIT: ndp_ns_input(ns_id, p); return;
    case ICMPV6_NB_ADVERT:  ndp_na_input(ns_id, p); return;
    case ICMPV6_RT_SOLICIT: ndp_rs_input(ns_id, p); return;
    case ICMPV6_RT_ADVERT:  ndp_ra_input(ns_id, p); return;
    default:
        /* Unsupported type: silent drop matches Linux for unknown ICMPv6. */
        return;
    }
}
