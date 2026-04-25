/* CosmoRT Network Stack — NIC bridge, global state, packet queues.
 * Protocol modules: tcp.c, udp.c, dispatch.c, arp.c, ip.c, dns.c, dhcp.c.
 */

#include "net/net.h"
#include "net/netif.h"
#include "net/net_util.h"
#include "hw/serial.h"
#include "core/tick.h"

#define NET_POLL_MAX_WORK   64

/* ── NIC Registration (legacy bridge to netif) ─────── */

static const nic_driver_t *nic;

/* netif wrapper for legacy nic_driver_t */
static struct netif hw_netif;

static void hw_send(struct netif *nif, const uint8_t *data, uint16_t len) {
    (void)nif;
    if (nic && nic->send) nic->send(data, len);
}

static void hw_get_mac(struct netif *nif, uint8_t *out) {
    (void)nif;
    if (nic && nic->get_mac) nic->get_mac(out);
}

void net_nic_register(const nic_driver_t *driver) {
    nic = driver;
    if (driver) {
        hw_netif.send = hw_send;
        hw_netif.get_mac = hw_get_mac;
        hw_netif.mtu = 1500;
        /* Copy driver name */
        const char *s = driver->name;
        int i = 0;
        while (s[i] && i < NETIF_NAME_MAX - 1) { hw_netif.name[i] = s[i]; i++; }
        hw_netif.name[i] = 0;
        netif_register(&hw_netif);
    }
}

const nic_driver_t *net_nic_get(void) { return nic; }

/* Network state */
net_state_t net_state = {{0}, {0}, {0}, {0}};

/* ── Packet Queues ─────────────────────────────────── */

pkt_queue_t q_tcp      = PKT_QUEUE_INIT;
pkt_queue_t q_udp_dhcp = PKT_QUEUE_INIT;
pkt_queue_t q_udp_dns  = PKT_QUEUE_INIT;
pkt_queue_t q_arp      = PKT_QUEUE_INIT;
pkt_queue_t q_icmp     = PKT_QUEUE_INIT;

struct thread *q_tcp_wait_thread;

void q_push(pkt_queue_t *q, const uint8_t *pkt, int len) {
    uint64_t flags;
    spin_lock_irq(&q->lock, &flags);
    if (q->count < Q_SIZE) {
        int idx = (q->head + q->count) % Q_SIZE;
        int l = len > Q_PKT ? Q_PKT : len;
        mcpy(q->data[idx], pkt, l);
        q->len[idx] = l;
        q->count++;
    }
    spin_unlock_irq(&q->lock, flags);
}

int q_pop(pkt_queue_t *q, uint8_t *buf, int bufsize) {
    uint64_t flags;
    spin_lock_irq(&q->lock, &flags);
    if (q->count == 0) {
        spin_unlock_irq(&q->lock, flags);
        return 0;
    }
    int l = q->len[q->head];
    if (l > bufsize) l = bufsize;
    mcpy(buf, q->data[q->head], l);
    q->head = (q->head + 1) % Q_SIZE;
    q->count--;
    spin_unlock_irq(&q->lock, flags);
    return l;
}

/* ── Init ──────────────────────────────────────────── */

extern void lo_init(void);

extern int net_rx_poll(int max_work);
extern int net_tx_poll(int max_work);

static void net_rx_tick(void) { (void)net_rx_poll(NET_POLL_MAX_WORK); }
static void net_tx_tick(void) { (void)net_tx_poll(NET_POLL_MAX_WORK); }

static struct tick_callback net_rx_cb;
static struct tick_callback net_tx_cb;

extern void ipv6_init(void);

int net_init(void) {
    lo_init();
    udp_init();
    ipv6_init();
    if (!nic) return -1;
    nic->get_mac(net_my_mac);
    tick_register(&net_rx_cb, net_rx_tick, TICK_EVERY);
    tick_register(&net_tx_cb, net_tx_tick, TICK_EVERY);
    return 0;
}
