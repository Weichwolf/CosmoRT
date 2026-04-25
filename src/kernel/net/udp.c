/* CosmoRT UDP — Hash-Table Demux, Slab Pool, Send/Recv
 * Extracted from net.c (Phase B).
 * E0: Sleep/Wake statt Busy-Wait.
 * Skal-C: Hash-table (64 buckets) + slab pool (128 slots).
 */

#include "net/net.h"
#include "net/net_ns.h"
#include "net/net_util.h"
#include "hw/serial.h"
#include "core/timer.h"
#include "core/event_queue.h"
#include "mm/slab.h"

/* ── Slab Pool (dynamic — grows on demand) ─────────── */

static slab_t udp_slab;
static int    udp_slab_inited;

static void udp_slab_ensure_init(void) {
    if (__builtin_expect(udp_slab_inited, 1)) return;
    slab_init_dynamic(&udp_slab, (int)sizeof(udp_sock_t), 1);
    udp_slab_inited = 1;
}

void udp_init(void) {
    udp_slab_ensure_init();
}

/* ── Hash Table (port → chain) ───────────────────────── */

static udp_sock_t *udp_hash[UDP_HASH_SIZE];
static spinlock_t  udp_table_lock = SPINLOCK_INIT;

/* ── Lookup (lock-free, called from RT-Core hot path) ── */

udp_sock_t *udp_find_ns(uint32_t ns_id, uint16_t port) {
    uint32_t idx = (port ^ ns_id) & (UDP_HASH_SIZE - 1);
    udp_sock_t *s = __atomic_load_n(&udp_hash[idx], __ATOMIC_ACQUIRE);
    while (s) {
        if (s->port == port && s->ns_id == ns_id) return s;
        s = s->hash_next;
    }
    return 0;
}

udp_sock_t *udp_find(uint16_t port) {
    return udp_find_ns(net_ns_current()->ns_id, port);
}

/* ── Bind / Unbind ───────────────────────────────────── */

udp_sock_t *udp_bind_ns(uint32_t ns_id, uint16_t port) {
    uint64_t flags;

    udp_slab_ensure_init();

    /* Pre-alloc outside table lock to avoid nested spinlocks */
    udp_sock_t *fresh = (udp_sock_t *)slab_alloc(&udp_slab);

    spin_lock_irq(&udp_table_lock, &flags);

    /* Duplicate check in hash chain */
    uint32_t idx = (port ^ ns_id) & (UDP_HASH_SIZE - 1);
    for (udp_sock_t *s = udp_hash[idx]; s; s = s->hash_next) {
        if (s->port == port && s->ns_id == ns_id) {
            spin_unlock_irq(&udp_table_lock, flags);
            if (fresh) slab_free(&udp_slab, fresh);
            return s; /* already bound — reuse */
        }
    }

    if (!fresh) {
        spin_unlock_irq(&udp_table_lock, flags);
        return 0; /* slab OOM */
    }

    fresh->port = port;
    fresh->ns_id = ns_id;
    fresh->q = (pkt_queue_t)PKT_QUEUE_INIT;
    fresh->wait_thread = 0;

    /* Insert at head of hash chain */
    fresh->hash_next = udp_hash[idx];
    __atomic_store_n(&udp_hash[idx], fresh, __ATOMIC_RELEASE);

    spin_unlock_irq(&udp_table_lock, flags);
    return fresh;
}

udp_sock_t *udp_bind(uint16_t port) {
    return udp_bind_ns(net_ns_current()->ns_id, port);
}

void udp_unbind(udp_sock_t *s) {
    if (!s) return;
    uint64_t flags;
    spin_lock_irq(&udp_table_lock, &flags);

    uint32_t idx = (s->port ^ s->ns_id) & (UDP_HASH_SIZE - 1);
    udp_sock_t **pp = &udp_hash[idx];
    while (*pp) {
        if (*pp == s) {
            *pp = s->hash_next;
            break;
        }
        pp = &(*pp)->hash_next;
    }

    s->port = 0;
    s->hash_next = 0;
    s->q.head = 0;
    s->q.count = 0;
    s->wait_thread = 0;

    spin_unlock_irq(&udp_table_lock, flags);

    slab_free(&udp_slab, s);
}

/* ── UDP Input (from dispatcher) ───────────────────── */

int udp_input(uint32_t ns_id, const uint8_t *pkt, int len) {
    if (len < 42) return 0;
    uint16_t dport = get16(pkt + 36);
    udp_sock_t *s = udp_find_ns(ns_id, dport);
    if (!s) return 0;
    q_push(&s->q, pkt, len);
    /* Wake thread blocked on recv */
    struct thread *wt = __atomic_load_n(&s->wait_thread, __ATOMIC_ACQUIRE);
    if (wt) event_post(wt, 8 /* EQ_SOCKET_DATA */, 0);
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
    (void)timeout_ms; /* timeout handled by caller via thread blocking */

    /* Auto-bind if not already registered */
    udp_sock_t *s = udp_find(local_port);
    if (!s) {
        s = udp_bind(local_port);
        if (!s) return -1;
    }

    uint8_t pkt[Q_PKT];
    int len = q_pop(&s->q, pkt, sizeof(pkt));
    if (len < 42) return -1; /* no data — caller blocks */

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
}
