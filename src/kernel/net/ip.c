/* CosmoRT IP — Header build, checksum, send (incl. loopback) */

#include "net/ip.h"
#include "net/net.h"
#include "net/net_util.h"
#include "core/rt.h"

extern const nic_driver_t *net_nic_get(void);

extern void tcp_input(const uint8_t *pkt, int len);
extern int  udp_input(const uint8_t *pkt, int len);

static void loopback_inject(const uint8_t *data, uint16_t len) {
    uint8_t lo[1600];
    if (len > 1600) return;
    for (int i = 0; i < len; i++) lo[i] = data[i];
    uint8_t proto = lo[23];
    if (proto == 6)       q_push(&q_tcp, lo, len);
    else if (proto == 1)  q_push(&q_icmp, lo, len);
    else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(lo + 36);
        if (dport == 68) q_push(&q_udp_dhcp, lo, len);
        else if (!udp_input(lo, len))
            q_push(&q_udp_dns, lo, len);
    }
}

void ip_send_raw(const uint8_t *data, uint16_t len) {
    if (len >= 34 && data[12] == 0x08 && data[13] == 0x00 && data[30] == 127) {
        loopback_inject(data, len);
        return;
    }

    if (rt_is_current_rt()) {
        const nic_driver_t *n = net_nic_get();
        if (n) n->send(data, len);
    } else {
        rt_channel_t *tx = net_tx_channel();
        if (tx) {
            rt_channel_push(tx, data, (uint32_t)len);
            extern void net_irq_thread_wake(void);
            net_irq_thread_wake();
        }
    }
}

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
