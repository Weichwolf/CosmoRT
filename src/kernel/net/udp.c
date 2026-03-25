/* CosmoRT UDP — Per-Socket Demux, Send/Recv
 * Extracted from net.c (Phase B).
 */

#include "net/net.h"
#include "net/net_util.h"
#include "hw/serial.h"
#include "core/timer.h"

/* ── Per-Socket Table ──────────────────────────────── */

static udp_sock_t udp_socks[NET_UDP_MAX];
static spinlock_t udp_table_lock = SPINLOCK_INIT;

udp_sock_t *udp_bind(uint16_t port) {
    uint64_t flags;
    spin_lock_irq(&udp_table_lock, &flags);
    /* Check for duplicate */
    for (int i = 0; i < NET_UDP_MAX; i++) {
        if (udp_socks[i].port == port) {
            spin_unlock_irq(&udp_table_lock, flags);
            return &udp_socks[i]; /* already bound — reuse */
        }
    }
    /* Find free slot */
    for (int i = 0; i < NET_UDP_MAX; i++) {
        if (udp_socks[i].port == 0) {
            udp_socks[i].port = port;
            udp_socks[i].q = (pkt_queue_t)PKT_QUEUE_INIT;
            spin_unlock_irq(&udp_table_lock, flags);
            return &udp_socks[i];
        }
    }
    spin_unlock_irq(&udp_table_lock, flags);
    return 0; /* table full */
}

void udp_unbind(udp_sock_t *s) {
    if (!s) return;
    uint64_t flags;
    spin_lock_irq(&udp_table_lock, &flags);
    s->port = 0;
    s->q.head = 0;
    s->q.count = 0;
    spin_unlock_irq(&udp_table_lock, flags);
}

udp_sock_t *udp_find(uint16_t port) {
    for (int i = 0; i < NET_UDP_MAX; i++) {
        if (__atomic_load_n(&udp_socks[i].port, __ATOMIC_ACQUIRE) == port)
            return &udp_socks[i];
    }
    return 0;
}

/* ── UDP Input (from dispatcher) ───────────────────── */

int udp_input(const uint8_t *pkt, int len) {
    if (len < 42) return 0;
    uint16_t dport = get16(pkt + 36);
    udp_sock_t *s = udp_find(dport);
    if (!s) return 0;
    q_push(&s->q, pkt, len);
    return 1;
}

int udp_poll_ready(uint16_t port) {
    udp_sock_t *s = udp_find(port);
    if (!s) return 0;
    return q_count(&s->q) > 0;
}

/* ── UDP Send ──────────────────────────────────────── */

int net_udp_send(const uint8_t *dst_ip, uint16_t dst_port,
                 uint16_t src_port, const void *data, int len) {
    if (len < 0 || len > 1400) return -1;

    uint8_t gw_mac[6];
    if (net_arp_resolve(net_gw_ip, gw_mac) < 0) return -1;

    uint8_t pkt[1536];
    mzero(pkt, sizeof(pkt));

    int udp_len = 8 + len;
    int ip_len  = 20 + udp_len;

    net_build_ip_hdr(pkt, gw_mac, dst_ip, 17, (uint16_t)udp_len);

    /* UDP header */
    put16(pkt + 34, src_port);
    put16(pkt + 36, dst_port);
    put16(pkt + 38, (uint16_t)udp_len);
    /* UDP checksum = 0 (optional for IPv4) */

    /* Payload */
    mcpy(pkt + 42, data, len);

    net_send_raw(pkt, (uint16_t)(14 + ip_len));
    return len;
}

/* ── UDP Recv ──────────────────────────────────────── */

int net_udp_recv(uint16_t local_port, void *buf, int bufsize,
                 uint8_t *src_ip_out, uint16_t *src_port_out,
                 int timeout_ms) {
    /* Auto-bind if not already registered */
    udp_sock_t *s = udp_find(local_port);
    if (!s) {
        s = udp_bind(local_port);
        if (!s) return -1;
    }

    uint8_t pkt[Q_PKT];
    uint64_t deadline = timer_ms() + (uint64_t)timeout_ms;

    /* Always run at least once (non-blocking callers pass timeout=0) */
    do {
        int len = q_pop(&s->q, pkt, sizeof(pkt));
        if (len < 42) {
            if (timeout_ms == 0) break;
            net_idle(); continue;
        }

        /* Extract payload */
        int ihl = (pkt[14] & 0x0F) * 4;
        int udp_off = 14 + ihl;
        int udp_len = get16(pkt + udp_off + 4);
        int data_off = udp_off + 8;
        int data_len = udp_len - 8;
        if (data_len < 0) data_len = 0;
        if (data_len > bufsize) data_len = bufsize;
        mcpy(buf, pkt + data_off, data_len);

        /* Source address */
        if (src_ip_out) mcpy(src_ip_out, pkt + 26, 4);
        if (src_port_out) *src_port_out = get16(pkt + udp_off);

        return data_len;
    } while (timer_ms() < deadline);
    return -1; /* timeout */
}
