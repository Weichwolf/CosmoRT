/* CosmoRT Packet Dispatcher — NIC RX → protocol demux */

#include "net/net.h"
#include "net/net_util.h"
#include "net/arp.h"

/* Forward declarations */
#include "net/net_ns.h"
extern void tcp_input(uint32_t ns_id, const uint8_t *pkt, int len);
extern int  udp_input(uint32_t ns_id, const uint8_t *pkt, int len);

/* NIC driver access (defined in net.c) */
extern const nic_driver_t *net_nic_get(void);

/* DNS local port (defined in dns.c) */
extern uint16_t dns_local_port;

/* ── Central Packet Dispatcher ─────────────────────── */

static volatile int net_poll_active;

static int net_rx_one(const nic_driver_t *n) {
    if (!n->recv) return 0;
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
    /* Wire packets always belong to init_net_ns — physical NICs are not
     * migrated into per-NS slots in Phase 15. */
    uint32_t wire_ns = init_net_ns.ns_id;
    if (proto == 6)       { tcp_input(wire_ns, pkt, len); queued = 1; }
    else if (proto == 1)  { q_push(&q_icmp, pkt, len); queued = 1; }
    else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(pkt + 36);
        if (dport == 68)
            q_push(&q_udp_dhcp, pkt, len);
        else if (dns_local_port && dport == dns_local_port)
            q_push(&q_udp_dns, pkt, len);
        else {
            udp_input(wire_ns, pkt, len);
        }
        queued = 1;
    }

    if (queued) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }
    return 1;
}

/* NIC IRQ entry point */
void net_poll(void) {
    const nic_driver_t *n = net_nic_get();
    if (!n) return;
    if (__sync_lock_test_and_set(&net_poll_active, 1)) return;
    net_rx_one(n);
    __sync_lock_release(&net_poll_active);
}

/* Timer IRQ handler: bounded RX polling */
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

/* Timer IRQ handler: TX is now direct (no ring to drain) */
int net_tx_poll(int max_work) {
    (void)max_work;
    return 0;
}
