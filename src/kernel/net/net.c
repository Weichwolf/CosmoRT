/* CosmoRT Network Stack — NIC registration, global state, packet queues */

#include "net/net.h"
#include "net/net_util.h"
#include "core/rt.h"
#include "core/irq_thread.h"
#include "hw/serial.h"

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

net_state_t net_state = {{0}, {0}, {0}, {0}};

pkt_queue_t q_tcp      = PKT_QUEUE_INIT;
pkt_queue_t q_udp_dhcp = PKT_QUEUE_INIT;
pkt_queue_t q_udp_dns  = PKT_QUEUE_INIT;
pkt_queue_t q_arp      = PKT_QUEUE_INIT;
pkt_queue_t q_icmp     = PKT_QUEUE_INIT;

struct thread *q_tcp_wait_thread;

struct thread *q_arp_wait_thread;

struct thread *q_dns_wait_thread;

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

static uint8_t tx_ring_buf[NET_TX_RING_SIZE]
    __attribute__((aligned(64)));
static rt_channel_t tx_ring;
static int tx_ring_ready;

rt_channel_t *net_tx_channel(void) {
    return tx_ring_ready ? &tx_ring : 0;
}

#define NET_THREAD_PRIO 90
#define NET_RX_BATCH    64

static irq_thread_t net_irq_thread;

static void net_thread_handler(void) {
    extern int net_rx_poll(int max_work);
    extern int net_tx_poll(int max_work);
    net_rx_poll(NET_RX_BATCH);
    net_tx_poll(NET_RX_BATCH);
}

void net_irq_thread_wake(void) {
    if (net_irq_thread.thread)
        irq_thread_wake(&net_irq_thread);
}

int net_init(void) {
    if (!nic) return -1;
    nic->get_mac(net_my_mac);
    rt_channel_init(&tx_ring, tx_ring_buf, NET_TX_RING_SIZE);
    tx_ring_ready = 1;

    irq_thread_create(&net_irq_thread, "net", net_thread_handler, NET_THREAD_PRIO);
    return 0;
}
