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

static int tx_ring_drain_impl(const nic_driver_t *n, int max_work) {
    rt_channel_t *tx = net_tx_channel();
    if (!tx) return 0;
    uint8_t frame[NET_PKT_SIZE];
    int len, count = 0;
    while ((max_work <= 0 || count < max_work) &&
           (len = rt_channel_pop(tx, frame, sizeof(frame))) > 0) {
        n->send(frame, (uint16_t)len);
        count++;
    }
    return count;
}

/* Called from timer IRQ — drain TX ring without full net_poll */
void net_tx_drain(void) {
    const nic_driver_t *n = net_nic_get();
    if (n) tx_ring_drain_impl(n, 0);
}

/* rt_poll handler: bounded TX drain. Returns work done. */
int net_tx_poll(int max_work) {
    const nic_driver_t *n = net_nic_get();
    if (!n) return 0;
    return tx_ring_drain_impl(n, max_work);
}

/* ── Central Packet Dispatcher ─────────────────────── */

static volatile int net_poll_active;

/* Process one RX packet. Returns 1 if packet processed, 0 if nothing. */
static int net_rx_one(const nic_driver_t *n) {
    uint8_t pkt[Q_PKT];
    int len = n->recv(pkt, sizeof(pkt));
    if (len < 14) return 0;
    uint16_t etype = get16(pkt + 12);
    if (etype == 0x0806) { arp_input(pkt, len); return 1; }
    if (etype != 0x0800 || len < 34) return 0;

    int ihl = (pkt[14] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl > len) return 0;
    int ip_total = get16(pkt + 16);
    if (ip_total < ihl || 14 + ip_total > len) return 0;

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
    return 1;
}

/* NIC IRQ entry point — unbounded, processes one RX + drains TX */
void net_poll(void) {
    const nic_driver_t *n = net_nic_get();
    if (!n) return;
    /* Reentrancy guard: timer IRQ can fire during net_poll */
    if (__sync_lock_test_and_set(&net_poll_active, 1)) return;

    /* Drain TX ring first — Compute-Cores may have queued frames */
    tx_ring_drain_impl(n, 0);

    net_rx_one(n);

    __sync_lock_release(&net_poll_active);
}

/* rt_poll handler: bounded RX. Returns number of packets processed. */
int net_rx_poll(int max_work) {
    const nic_driver_t *n = net_nic_get();
    if (!n) return 0;
    if (__sync_lock_test_and_set(&net_poll_active, 1)) return 0;

    int count = 0;
    while ((max_work <= 0 || count < max_work) && net_rx_one(n))
        count++;

    __sync_lock_release(&net_poll_active);
    return count;
}
