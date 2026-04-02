/* CosmoRT IP — Header build, checksum, send.
 * Routing via netif_tx() (loopback handled by netif layer).
 */

#include "net/ip.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/net_util.h"

/* ── IP Header Build ───────────────────────────────── */

void ip_build_header_tos(uint8_t *pkt, const uint8_t *dst_mac,
                         const uint8_t *dst_ip, uint8_t proto,
                         uint16_t plen, uint8_t tos) {
    mcpy(pkt, dst_mac, 6); mcpy(pkt + 6, net_my_mac, 6); put16(pkt + 12, 0x0800);
    pkt[14] = 0x45; pkt[15] = tos; put16(pkt + 16, 20 + plen);
    put16(pkt + 18, 0); put16(pkt + 20, 0x4000);
    pkt[22] = 64; pkt[23] = proto; pkt[24] = 0; pkt[25] = 0;
    mcpy(pkt + 26, net_my_ip, 4); mcpy(pkt + 30, dst_ip, 4);
    uint16_t ic = ip_cksum(pkt + 14, 20);
    pkt[24] = (uint8_t)(ic >> 8); pkt[25] = (uint8_t)ic;
}

void ip_build_header(uint8_t *pkt, const uint8_t *dst_mac,
                     const uint8_t *dst_ip, uint8_t proto, uint16_t plen) {
    ip_build_header_tos(pkt, dst_mac, dst_ip, proto, plen, 0);
}

/* ── IP Send ───────────────────────────────────────── */

void ip_send_raw(const uint8_t *data, uint16_t len) {
    netif_tx(data, len);
}

/* ── Compat wrappers (used by tcp.c, udp.c, arp.c) ── */

void net_send_raw(const uint8_t *data, uint16_t len) { ip_send_raw(data, len); }

void net_build_ip_hdr(uint8_t *pkt, const uint8_t *dst_mac,
                       const uint8_t *dst_ip, uint8_t proto, uint16_t plen) {
    ip_build_header(pkt, dst_mac, dst_ip, proto, plen);
}

void net_build_ip_hdr_tos(uint8_t *pkt, const uint8_t *dst_mac,
                           const uint8_t *dst_ip, uint8_t proto,
                           uint16_t plen, uint8_t tos) {
    ip_build_header_tos(pkt, dst_mac, dst_ip, proto, plen, tos);
}
