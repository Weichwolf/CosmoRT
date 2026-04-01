/* CosmoRT Network Stack — NIC registration, global state, packet queues.
 * Protocol modules: tcp.c, udp.c, dispatch.c, arp.c, ip.c, dns.c, dhcp.c.
 */

#include "net/net.h"
#include "net/net_util.h"
#include "hw/serial.h"

/* ── NIC Registration ──────────────────────────────── */

static const nic_driver_t *nic;

void net_nic_register(const nic_driver_t *driver) {
    nic = driver;
    if (driver) {
        serial_puts("net: NIC registered: ");
        serial_puts(driver->name);
        serial_putchar('\n');
    } else {
        serial_puts("net: NIC deregistered\n");
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

int net_init(void) {
    if (!nic) return -1;
    nic->get_mac(net_my_mac);
    return 0;
}
