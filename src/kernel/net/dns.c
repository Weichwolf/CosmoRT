/* CosmoRT DNS Resolver — kernel-level DNS for boot
 * Extracted from net.c (Phase D1).
 */

#include "net/dns.h"
#include "net/net.h"
#include "net/net_util.h"
#include "net/ip.h"
#include "net/arp.h"
#include "core/timer.h"
#include "core/event_queue.h"
#include "proc/process.h"

uint16_t dns_local_port = 0;

int net_dns_resolve(const char *hostname, uint8_t ip_out[4]) {
    if (net_dns_ip[0] == 0) {
        if (net_gw_ip[0]) mcpy(net_dns_ip, net_gw_ip, 4);
        else return -1;
    }

    uint8_t gw_mac[6];
    if (net_arp_resolve(net_gw_ip, gw_mac) < 0) return -1;

    {
        extern int random_get(void *, unsigned long);
        uint16_t rnd16;
        if (random_get(&rnd16, sizeof(rnd16)) < 0)
            rnd16 = (uint16_t)(timer_ms() & 0xFFFF);
        dns_local_port = (uint16_t)(49152 + (rnd16 & 0x3FFF));
    }

    uint8_t pkt[256]; mzero(pkt, sizeof(pkt));
    mcpy(pkt, gw_mac, 6); mcpy(pkt+6, net_my_mac, 6);
    put16(pkt+12, 0x0800);
    pkt[14] = 0x45; pkt[22] = 64; pkt[23] = 17;
    mcpy(pkt+26, net_my_ip, 4);
    mcpy(pkt+30, net_dns_ip, 4);
    put16(pkt+34, dns_local_port); put16(pkt+36, 53);

    uint8_t *dns = pkt + 42;
    uint16_t txid;
    {
        extern int random_get(void *, unsigned long);
        if (random_get(&txid, sizeof(txid)) < 0)
            txid = (uint16_t)(timer_ms() & 0xFFFF);
    }
    put16(dns, txid);
    put16(dns+2, 0x0100);
    put16(dns+4, 1);
    put16(dns+6, 0); put16(dns+8, 0); put16(dns+10, 0);

    int qi = 12;
    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - p);
        if (label_len > 63 || qi + label_len + 1 > 200) return -1;
        dns[qi++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) dns[qi++] = (uint8_t)p[i];
        p = *dot ? dot + 1 : dot;
    }
    dns[qi++] = 0;
    put16(dns+qi, 1); qi += 2;
    put16(dns+qi, 1); qi += 2;

    int dns_len = qi;
    int udp_len = 8 + dns_len;
    int ip_len = 20 + udp_len;
    put16(pkt+16, (uint16_t)ip_len);
    put16(pkt+38, (uint16_t)udp_len);
    uint16_t ic = ip_cksum(pkt+14, 20);
    pkt[24] = (uint8_t)(ic >> 8); pkt[25] = (uint8_t)ic;

    ip_send_raw(pkt, (uint16_t)(14 + ip_len));

    uint8_t reply[Q_PKT];
    uint64_t deadline = timer_ms() + NET_DHCP_RETRY_MS;
    thread_t *cur = thread_current();
    if (cur)
        __atomic_store_n(&q_dns_wait_thread, cur, __ATOMIC_RELEASE);

    while (timer_ms() < deadline) {
        int len = q_pop(&q_udp_dns, reply, sizeof(reply));
        if (len < 42 + 12) {
            if (cur) {
                int remain = (int)(deadline - timer_ms());
                if (remain <= 0) break;
                event_t ev;
                event_wait(&cur->eq, &ev, remain);
            } else {
                arch_halt();
            }
            continue;
        }

        uint8_t *rdns = reply + 42;
        if (get16(rdns) != txid) continue;
        uint16_t flags = get16(rdns+2);
        if (!(flags & 0x8000)) continue;
        if ((flags & 0x000F) != 0) { dns_local_port = 0; return -1; }

        int ancount = get16(rdns+6);
        int ri2 = 12;
        int rdns_len = len - 42;
        while (ri2 < rdns_len && rdns[ri2] != 0) {
            if ((rdns[ri2] & 0xC0) == 0xC0) { ri2 += 2; break; }
            int lbl = rdns[ri2] + 1;
            if (ri2 + lbl > rdns_len) break;
            ri2 += lbl;
        }
        if (ri2 < rdns_len && rdns[ri2] == 0) ri2++;
        if (ri2 + 4 > rdns_len) continue;
        ri2 += 4;

        for (int a = 0; a < ancount && ri2 + 12 <= rdns_len; a++) {
            if ((rdns[ri2] & 0xC0) == 0xC0) { ri2 += 2; if (ri2 > rdns_len) break; }
            else { while (ri2 < rdns_len && rdns[ri2]) { int l = rdns[ri2]+1; if (ri2+l > rdns_len) break; ri2 += l; } if (ri2 >= rdns_len) break; ri2++; }
            if (ri2 + 10 > rdns_len) break;
            uint16_t rtype = get16(rdns+ri2); ri2 += 2;
            ri2 += 2; ri2 += 4;
            uint16_t rdlen = get16(rdns+ri2); ri2 += 2;
            if (rtype == 1 && rdlen == 4 && ri2 + 4 <= rdns_len) {
                mcpy(ip_out, rdns+ri2, 4);
                dns_local_port = 0;
                if (cur)
                    __atomic_store_n(&q_dns_wait_thread, (struct thread *)0, __ATOMIC_RELEASE);
                return 0;
            }
            ri2 += rdlen;
        }
    }
    dns_local_port = 0;
    if (cur)
        __atomic_store_n(&q_dns_wait_thread, (struct thread *)0, __ATOMIC_RELEASE);
    return -1;
}
