/* CosmoRT Packet Dispatcher — NIC RX → protocol demux
 * Extracted from net.c (Phase C).
 */

#include "net/net.h"
#include "net/net_util.h"
#include "net/arp.h"
#include "core/rt.h"

/* Forward declarations */
extern void tcp_input(const uint8_t *pkt, int len);
extern int  udp_input(const uint8_t *pkt, int len);

/* NIC driver access (defined in net.c) */
extern const nic_driver_t *net_nic_get(void);

/* DNS local port (defined in dns.c, set by DNS resolver) */
extern uint16_t dns_local_port;

/* ── TX Ring Drain (Compute→RT) ────────────────────── */

static void tx_ring_drain_impl(const nic_driver_t *n) {
    rt_channel_t *tx = net_tx_channel();
    if (!tx) return;
    uint8_t frame[NET_PKT_SIZE];
    int len;
    while ((len = rt_channel_pop(tx, frame, sizeof(frame))) > 0)
        n->send(frame, (uint16_t)len);
}

/* Called from timer IRQ — drain TX ring without full net_poll */
void net_tx_drain(void) {
    const nic_driver_t *n = net_nic_get();
    if (n) tx_ring_drain_impl(n);
}

/* ── Central Packet Dispatcher ─────────────────────── */

static volatile int net_poll_active;

void net_poll(void) {
    const nic_driver_t *n = net_nic_get();
    if (!n) return;
    /* Reentrancy guard: timer IRQ can fire during net_poll */
    if (__sync_lock_test_and_set(&net_poll_active, 1)) return;

    /* Drain TX ring first — Compute-Cores may have queued frames */
    tx_ring_drain_impl(n);

    uint8_t pkt[Q_PKT];
    int len = n->recv(pkt, sizeof(pkt));
    if (len < 14) goto out;
    uint16_t etype = get16(pkt + 12);
    if (etype == 0x0806) { arp_input(pkt, len); goto out; }
    if (etype != 0x0800 || len < 34) goto out;

    int ihl = (pkt[14] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl > len) goto out;
    int ip_total = get16(pkt + 16);
    if (ip_total < ihl || 14 + ip_total > len) goto out;

    uint8_t proto = pkt[23];
    int queued = 0;
    if (proto == 6)       { tcp_input(pkt, len); queued = 1; }
    else if (proto == 1)  { q_push(&q_icmp, pkt, len); queued = 1; }
    else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(pkt + 36);
        if (dport == 68)
            q_push(&q_udp_dhcp, pkt, len);
        else if (dns_local_port && dport == dns_local_port)
            q_push(&q_udp_dns, pkt, len);
        else {
            udp_input(pkt, len);
        }
        queued = 1;
    }

    /* Wake epoll sleepers when new packets arrive */
    if (queued) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }

out:
    __sync_lock_release(&net_poll_active);
}
