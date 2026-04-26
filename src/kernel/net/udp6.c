/* CosmoRT UDP6 — IPv6 UDP socket layer.
 *
 * Separate hash table from v4 udp.c so the namespace key (ns_id, port)
 * doesn't collide between protocol families. udp_sock_t is reused as-is
 * (it's protocol-agnostic — just a port+queue+wait pair). */

#include "net/udp6.h"
#include "net/udp.h"
#include "net/in6.h"
#include "net/ipv6.h"
#include "net/icmpv6.h"
#include "net/net.h"
#include "net/net_util.h"
#include "core/timer.h"
#include "core/waitqueue.h"
#include "mm/slab.h"

extern int ndp_resolve(uint32_t ns_id, const struct in6_addr *addr, uint8_t *mac_out);

#define UDP6_HASH_SIZE 64

static udp_sock_t *udp6_hash[UDP6_HASH_SIZE];
static spinlock_t  udp6_hash_lock = SPINLOCK_INIT;
static slab_t      udp6_slab;
static int         udp6_inited;

static void udp6_slab_ensure(void) {
    if (__builtin_expect(udp6_inited, 1)) return;
    slab_init_dynamic(&udp6_slab, (int)sizeof(udp_sock_t), 1);
    udp6_inited = 1;
}

void udp6_init(void) { udp6_slab_ensure(); }

static udp_sock_t *udp6_find(uint32_t ns_id, uint16_t port) {
    uint32_t idx = (port ^ ns_id) & (UDP6_HASH_SIZE - 1);
    udp_sock_t *s = __atomic_load_n(&udp6_hash[idx], __ATOMIC_ACQUIRE);
    while (s) {
        if (s->port == port && s->ns_id == ns_id) return s;
        s = s->hash_next;
    }
    return 0;
}

static udp_sock_t *udp6_bind(uint32_t ns_id, uint16_t port) {
    uint64_t flags;
    udp6_slab_ensure();
    udp_sock_t *fresh = (udp_sock_t *)slab_alloc(&udp6_slab);
    spin_lock_irq(&udp6_hash_lock, &flags);
    uint32_t idx = (port ^ ns_id) & (UDP6_HASH_SIZE - 1);
    for (udp_sock_t *s = udp6_hash[idx]; s; s = s->hash_next) {
        if (s->port == port && s->ns_id == ns_id) {
            spin_unlock_irq(&udp6_hash_lock, flags);
            if (fresh) slab_free(&udp6_slab, fresh);
            return s;
        }
    }
    if (!fresh) {
        spin_unlock_irq(&udp6_hash_lock, flags);
        return 0;
    }
    fresh->port = port;
    fresh->ns_id = ns_id;
    fresh->q = (pkt_queue_t)PKT_QUEUE_INIT;
    init_waitqueue_head(&fresh->recv_wq);
    fresh->hash_next = udp6_hash[idx];
    __atomic_store_n(&udp6_hash[idx], fresh, __ATOMIC_RELEASE);
    spin_unlock_irq(&udp6_hash_lock, flags);
    return fresh;
}

/* ── Checksum (RFC 8200 §8.1 same form as TCP6) ────── */

static uint16_t udp6_cksum(const struct in6_addr *src, const struct in6_addr *dst,
                            const uint8_t *udp, int ulen) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)((src->s6_addr[i] << 8) | src->s6_addr[i + 1]);
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)((dst->s6_addr[i] << 8) | dst->s6_addr[i + 1]);
    sum += (uint32_t)((ulen >> 16) & 0xFFFF);
    sum += (uint32_t)(ulen & 0xFFFF);
    sum += 17; /* next header = UDP */
    for (int i = 0; i < ulen; i += 2) {
        uint16_t w = (uint16_t)(udp[i] << 8);
        if (i + 1 < ulen) w |= udp[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Send ──────────────────────────────────────────── */

int udp6_send(uint32_t ns_id, const struct in6_addr *dst, uint16_t dst_port,
              uint16_t src_port, const void *data, int len) {
    if (len < 0 || len > 1400) return -1;

    /* Resolve dst MAC. */
    uint8_t dst_mac[6];
    if (in6_is_loopback(dst)) {
        for (int i = 0; i < 6; i++) dst_mac[i] = 0;
    } else if (ndp_resolve(ns_id, dst, dst_mac) < 0) {
        return -1;
    }

    /* Pick source. */
    struct in6_addr src;
    ipv6_select_src(ns_id, dst, &src);

    int udp_len = 8 + len;
    uint8_t pkt[1536];
    mzero(pkt, sizeof(pkt));

    ipv6_build_header(pkt, dst_mac, &src, dst, IPPROTO_UDP_V6,
                      (uint16_t)udp_len, IPV6_DEFAULT_HOPLIM);

    uint8_t *u = pkt + 54;
    put16(u,     src_port);
    put16(u + 2, dst_port);
    put16(u + 4, (uint16_t)udp_len);
    put16(u + 6, 0);
    mcpy(u + 8, data, len);
    /* RFC 8200: UDP cksum is mandatory in IPv6 (unlike v4). */
    uint16_t c = udp6_cksum(&src, dst, u, udp_len);
    if (c == 0) c = 0xFFFF;
    u[6] = (uint8_t)(c >> 8);
    u[7] = (uint8_t)c;

    ipv6_send_frame(pkt, (uint16_t)(54 + udp_len));
    return len;
}

/* ── Recv ──────────────────────────────────────────── */

int udp6_recv(uint32_t ns_id, uint16_t local_port,
              void *buf, int bufsize,
              struct in6_addr *src_out, uint16_t *src_port_out,
              int timeout_ms) {
    (void)timeout_ms;
    udp_sock_t *s = udp6_find(ns_id, local_port);
    if (!s) {
        s = udp6_bind(ns_id, local_port);
        if (!s) return -1;
    }
    uint8_t pkt[Q_PKT];
    int len = q_pop(&s->q, pkt, sizeof(pkt));
    if (len < 54 + 8) return -1;

    /* Frame layout: 14 eth + 40 v6 + 8 UDP + payload. */
    int udp_off = 54;
    int udp_len = get16(pkt + udp_off + 4);
    int data_off = udp_off + 8;
    int data_len = udp_len - 8;
    if (data_len < 0) data_len = 0;
    if (data_len > bufsize) data_len = bufsize;
    mcpy(buf, pkt + data_off, data_len);

    if (src_out)      for (int i = 0; i < 16; i++) src_out->s6_addr[i] = pkt[14 + 8 + i];
    if (src_port_out) *src_port_out = get16(pkt + udp_off);
    return data_len;
}

/* ── Demux ─────────────────────────────────────────── */

int udp6_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    if (p->l4_len < 8) return 0;
    const uint8_t *u = p->frame + p->l4_off;
    uint16_t dport = get16(u + 2);
    udp_sock_t *s = udp6_find(ns_id, dport);
    if (!s) {
        /* Optional ICMPv6 port-unreachable; loopback can skip. */
        if (!in6_is_loopback(&p->dst)) icmpv6_send_port_unreach(ns_id, p);
        return 0;
    }
    /* Push the FULL frame so udp6_recv can extract addresses + payload. */
    q_push(&s->q, p->frame, p->frame_len);
    wake_up(&s->recv_wq);
    return 1;
}

/* Bind helpers exported for socket.c. */
udp_sock_t *udp6_bind_ns(uint32_t ns_id, uint16_t port) {
    return udp6_bind(ns_id, port);
}

udp_sock_t *udp6_find_ns(uint32_t ns_id, uint16_t port) {
    return udp6_find(ns_id, port);
}

void udp6_unbind(udp_sock_t *s) {
    if (!s) return;
    uint64_t flags;
    spin_lock_irq(&udp6_hash_lock, &flags);
    uint32_t idx = (s->port ^ s->ns_id) & (UDP6_HASH_SIZE - 1);
    udp_sock_t **pp = &udp6_hash[idx];
    while (*pp) {
        if (*pp == s) { *pp = s->hash_next; break; }
        pp = &(*pp)->hash_next;
    }
    s->port = 0; s->hash_next = 0; s->q.head = 0; s->q.count = 0;
    wake_up_all(&s->recv_wq);
    spin_unlock_irq(&udp6_hash_lock, flags);
    slab_free(&udp6_slab, s);
}
