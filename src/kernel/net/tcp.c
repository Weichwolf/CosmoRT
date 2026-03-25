/* CosmoRT TCP — Per-Socket Ringbuffer, State-Machine, Hash-Lookup
 * Extracted from net.c (Phase A).
 */

#include "tcp.h"
#include "net.h"
#include "net_util.h"
#include "timer.h"
#include "page_alloc.h"

/* ── Ringbuffer ────────────────────────────────────── */

int rxring_init(tcp_rxring_t *r) {
    r->head = 0;
    r->tail = 0;
    r->lock = (spinlock_t)SPINLOCK_INIT;
    int npages = (NET_TCP_RXBUF + 4095) / 4096;
    r->buf = (uint8_t *)pages_alloc(npages);
    if (!r->buf) return -12; /* -ENOMEM */
    return 0;
}

void rxring_destroy(tcp_rxring_t *r) {
    if (r->buf) {
        int npages = (NET_TCP_RXBUF + 4095) / 4096;
        pages_free(r->buf, npages);
        r->buf = 0;
    }
    r->head = r->tail = 0;
}

int rxring_used(const tcp_rxring_t *r) {
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return (int)((t - h) & (NET_TCP_RXBUF - 1));
}

int rxring_free(const tcp_rxring_t *r) {
    return NET_TCP_RXBUF - 1 - rxring_used(r);
}

int rxring_push(tcp_rxring_t *r, const void *data, int len) {
    if (!r->buf) return 0;
    uint64_t flags;
    spin_lock_irq(&r->lock, &flags);
    int avail = NET_TCP_RXBUF - 1 - (int)((r->tail - r->head) & (NET_TCP_RXBUF - 1));
    if (len > avail) len = avail;
    if (len <= 0) { spin_unlock_irq(&r->lock, flags); return 0; }

    const uint8_t *src = data;
    uint32_t mask = NET_TCP_RXBUF - 1;
    for (int i = 0; i < len; i++)
        r->buf[(r->tail + (uint32_t)i) & mask] = src[i];
    r->tail = (r->tail + (uint32_t)len) & mask;
    spin_unlock_irq(&r->lock, flags);
    return len;
}

int rxring_pop(tcp_rxring_t *r, void *buf, int len) {
    if (!r->buf) return 0;
    uint64_t flags;
    spin_lock_irq(&r->lock, &flags);
    int used = (int)((r->tail - r->head) & (NET_TCP_RXBUF - 1));
    if (len > used) len = used;
    if (len <= 0) { spin_unlock_irq(&r->lock, flags); return 0; }

    uint8_t *dst = buf;
    uint32_t mask = NET_TCP_RXBUF - 1;
    for (int i = 0; i < len; i++)
        dst[i] = r->buf[(r->head + (uint32_t)i) & mask];
    r->head = (r->head + (uint32_t)len) & mask;
    spin_unlock_irq(&r->lock, flags);
    return len;
}

/* ── TCP Hash Table ────────────────────────────────── */

#define TCP_HASH_SIZE 64  /* must be power of 2 */

static net_tcp_t *tcp_hash[TCP_HASH_SIZE];
static spinlock_t tcp_hash_lock = SPINLOCK_INIT;

static uint32_t tcp_hash_fn(uint16_t lport, uint16_t rport, const uint8_t *sip) {
    uint32_t h = (uint32_t)lport ^ ((uint32_t)rport << 7);
    h ^= (uint32_t)sip[0] ^ ((uint32_t)sip[1] << 8) ^
         ((uint32_t)sip[2] << 16) ^ ((uint32_t)sip[3] << 24);
    h ^= h >> 16;
    return h & (TCP_HASH_SIZE - 1);
}

void tcp_register(net_tcp_t *c) {
    uint32_t idx = tcp_hash_fn(c->local_port, c->remote_port, c->dst_ip);
    uint64_t flags;
    spin_lock_irq(&tcp_hash_lock, &flags);
    tcp_hash[idx] = c;  /* simple: one slot per bucket, last wins */
    spin_unlock_irq(&tcp_hash_lock, flags);
}

void tcp_unregister(net_tcp_t *c) {
    uint32_t idx = tcp_hash_fn(c->local_port, c->remote_port, c->dst_ip);
    uint64_t flags;
    spin_lock_irq(&tcp_hash_lock, &flags);
    if (tcp_hash[idx] == c)
        tcp_hash[idx] = 0;
    spin_unlock_irq(&tcp_hash_lock, flags);
}

net_tcp_t *tcp_find(uint16_t local_port, uint16_t remote_port, const uint8_t *src_ip) {
    uint32_t idx = tcp_hash_fn(local_port, remote_port, src_ip);
    return __atomic_load_n(&tcp_hash[idx], __ATOMIC_ACQUIRE);
}

/* ── TCP Checksum ──────────────────────────────────── */

static uint16_t tcp_cksum(const uint8_t *sip, const uint8_t *dip,
                           const uint8_t *tcp, int tlen) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2)
        sum += (uint32_t)((sip[i] << 8) | sip[i + 1]);
    for (int i = 0; i < 4; i += 2)
        sum += (uint32_t)((dip[i] << 8) | dip[i + 1]);
    sum += 6;
    sum += (uint32_t)tlen;
    for (int i = 0; i < tlen; i += 2) {
        uint16_t w = (uint16_t)(tcp[i] << 8);
        if (i + 1 < tlen) w |= tcp[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Send TCP Segment ──────────────────────────────── */

static void send_tcp(net_tcp_t *c, uint8_t flags, const void *data, int dlen) {
    uint8_t pkt[1536];
    int thdr = 20, tt = thdr + dlen;
    mzero(pkt, 54 + dlen);
    net_build_ip_hdr(pkt, c->dst_mac, c->dst_ip, 6, (uint16_t)tt);
    uint8_t *t = pkt + 34;
    put16(t, c->local_port);
    put16(t + 2, c->remote_port);
    put32(t + 4, c->snd_nxt);
    put32(t + 8, c->rcv_nxt);
    t[12] = (uint8_t)((thdr / 4) << 4);
    t[13] = flags;
    put16(t + 14, c->rcv_wnd ? c->rcv_wnd : 8192);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (dlen > 0 && data) mcpy(t + thdr, data, dlen);
    uint16_t ck = tcp_cksum(net_my_ip, c->dst_ip, t, tt);
    t[16] = (uint8_t)(ck >> 8);
    t[17] = (uint8_t)ck;
    net_send_raw(pkt, (uint16_t)(34 + tt));
}

/* ── TCP Input (from dispatcher) ───────────────────── */

void tcp_input(const uint8_t *pkt, int len) {
    if (len < 54) return;

    uint16_t dport = get16(pkt + 36);
    uint16_t sport = get16(pkt + 34);
    const uint8_t *src_ip = pkt + 26;

    net_tcp_t *c = tcp_find(dport, sport, src_ip);
    if (!c) {
        /* No connection found — push to global q_tcp for accept/connect */
        q_push(&q_tcp, pkt, len);
        return;
    }

    uint8_t flags = pkt[47];
    int doff = (pkt[46] >> 4) * 4;
    if (doff < 20) doff = 20;
    int ip_total = get16(pkt + 16);
    if (ip_total < 20 + doff) return;
    int plen = ip_total - 20 - doff;
    if (14 + 20 + doff + plen > len) plen = len - 14 - 20 - doff;
    if (plen < 0) plen = 0;
    uint32_t tseq = get32(pkt + 38);

    /* RST → connection reset */
    if (flags & 0x04) {
        c->state = TCP_CLOSED;
        c->got_rst = 1;
        return;
    }

    /* Data */
    if (plen > 0) {
        if (tseq != c->rcv_nxt) {
            /* Out-of-order: ACK to trigger retransmit */
            send_tcp(c, 0x10, 0, 0);
            return;
        }
        c->rcv_nxt = tseq + (uint32_t)plen;
        const uint8_t *payload = pkt + 14 + 20 + doff;
        rxring_push(&c->rx, payload, plen);
        send_tcp(c, 0x10, 0, 0);
    }

    /* FIN */
    if (flags & 0x01) {
        if (plen == 0 && tseq != c->rcv_nxt) return;
        c->got_fin = 1;
        c->rcv_nxt++;
        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_CLOSE_WAIT;
            send_tcp(c, 0x10, 0, 0);  /* ACK the FIN */
        } else if (c->state == TCP_FIN_WAIT1) {
            c->state = TCP_CLOSING;
            send_tcp(c, 0x10, 0, 0);
        } else if (c->state == TCP_FIN_WAIT2) {
            c->state = TCP_TIME_WAIT;
            send_tcp(c, 0x10, 0, 0);
        }
    }

    /* ACK processing for close states */
    if (flags & 0x10) {
        uint32_t ack_num = get32(pkt + 42);
        if (c->state == TCP_FIN_WAIT1 && ack_num == c->snd_nxt) {
            c->state = TCP_FIN_WAIT2;
        } else if (c->state == TCP_CLOSING && ack_num == c->snd_nxt) {
            c->state = TCP_TIME_WAIT;
        } else if (c->state == TCP_LAST_ACK && ack_num == c->snd_nxt) {
            c->state = TCP_CLOSED;
            tcp_unregister(c);
        }
        /* Track snd_una */
        if ((int32_t)(ack_num - c->snd_una) > 0)
            c->snd_una = ack_num;
    }
}

/* ── TCP Accept ────────────────────────────────────── */

int net_tcp_accept(net_tcp_t *c, uint16_t local_port, int timeout_ms) {
    uint8_t pkt[Q_PKT];
    uint64_t deadline = timer_ms() + (uint64_t)timeout_ms;

    /* Phase 1: wait for SYN on local_port */
    while (timer_ms() < deadline) {
        int len = q_pop(&q_tcp, pkt, sizeof(pkt));
        if (len < 54) { net_idle(); continue; }
        uint16_t dport = get16(pkt + 36);
        if (dport != local_port) continue;
        uint8_t fl = pkt[47];
        if ((fl & 0x02) && !(fl & 0x10)) break; /* SYN only */
    }
    if (timer_ms() >= deadline) return -1;

    /* Initialize connection from SYN */
    mzero(c, sizeof(*c));
    rxring_init(&c->rx);
    mcpy(c->dst_mac, pkt + 6, 6);
    mcpy(c->dst_ip, pkt + 26, 4);
    c->remote_port = get16(pkt + 34);
    c->local_port = get16(pkt + 36);
    uint32_t client_isn = get32(pkt + 38);
    c->rcv_nxt = client_isn + 1;
    uint32_t rseq;
    net_random(&rseq, (int)sizeof(rseq));
    c->snd_nxt = rseq;
    c->snd_una = rseq;
    c->rcv_wnd = 8192;

    /* Send SYN-ACK */
    send_tcp(c, 0x12, 0, 0);
    c->state = TCP_SYN_RCVD;

    /* Phase 2: wait for ACK */
    deadline = timer_ms() + (uint64_t)timeout_ms;
    while (timer_ms() < deadline) {
        int len = q_pop(&q_tcp, pkt, sizeof(pkt));
        if (len < 54) { net_idle(); continue; }
        if (get16(pkt + 36) != c->local_port) continue;
        if (get16(pkt + 34) != c->remote_port) continue;
        uint8_t fl = pkt[47];
        if (fl & 0x04) { c->state = TCP_CLOSED; return -1; } /* RST */
        if (fl & 0x10) {
            uint32_t ack_num = get32(pkt + 42);
            if (ack_num == c->snd_nxt + 1) {
                c->snd_nxt++;
                c->snd_una = c->snd_nxt;
                c->state = TCP_ESTABLISHED;
                tcp_register(c);
                return 0;
            }
        }
    }
    c->state = TCP_CLOSED;
    return -1;
}

/* ── TCP Connect ───────────────────────────────────── */

int net_tcp_connect(net_tcp_t *c, const uint8_t *dst_ip, uint16_t port) {
    mzero(c, sizeof(*c));
    rxring_init(&c->rx);
    mcpy(c->dst_ip, dst_ip, 4);
    c->remote_port = port;
    uint32_t rseq;
    net_random(&rseq, (int)sizeof(rseq));
    uint16_t rport;
    net_random(&rport, (int)sizeof(rport));
    c->local_port = (uint16_t)((rport % 16384) + 49152);
    c->snd_nxt = rseq;
    c->snd_una = rseq;
    c->rcv_nxt = 0;
    c->rcv_wnd = 8192;
    c->state = TCP_CLOSED;

    if (net_arp_resolve(net_gw_ip, c->dst_mac) < 0) return -1;

    send_tcp(c, 0x02, 0, 0);
    c->state = TCP_SYN_SENT;

    uint8_t reply[Q_PKT];
    uint64_t deadline = timer_ms() + NET_TCP_TIMEOUT_MS;
    while (timer_ms() < deadline) {
        int len = q_pop(&q_tcp, reply, sizeof(reply));
        if (len < 54) { net_idle(); continue; }
        if (get16(reply + 36) != c->local_port) continue;
        if (get16(reply + 34) != c->remote_port) continue;
        uint8_t fl = reply[47];
        if ((fl & 0x12) == 0x12) {
            c->rcv_nxt = get32(reply + 38) + 1;
            c->snd_nxt++;
            c->snd_una = c->snd_nxt;
            send_tcp(c, 0x10, 0, 0);
            c->state = TCP_ESTABLISHED;
            tcp_register(c);
            return 0;
        }
        if (fl & 0x04) return -1;
    }
    return -1;
}

/* ── TCP Send ──────────────────────────────────────── */

int net_tcp_send(net_tcp_t *c, const void *data, int len) {
    if (c->state != TCP_ESTABLISHED) return -1;
    const uint8_t *p = data;
    int sent = 0;
    while (sent < len) {
        int ch = len - sent;
        if (ch > 1400) ch = 1400;
        send_tcp(c, 0x18, p + sent, ch);
        c->snd_nxt += (uint32_t)ch;
        sent += ch;
    }
    return sent;
}

/* ── TCP Recv ──────────────────────────────────────── */

int net_tcp_recv(net_tcp_t *c, void *buf, int bufsize, int timeout_ms) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        if (c->state == TCP_CLOSED && (c->got_fin || c->got_rst)) {
            /* Connection already closed — drain ringbuffer */
            int n = rxring_pop(&c->rx, buf, bufsize);
            return n > 0 ? n : (c->got_rst ? -1 : 0);
        }
        return -1;
    }

    int total = 0;
    uint64_t last_data_ms = timer_ms();
    uint64_t tmo = (uint64_t)timeout_ms;

    /* Drain ringbuffer first */
    int got = rxring_pop(&c->rx, buf, bufsize);
    if (got > 0) return got;

    /* Poll for incoming data — dispatcher puts packets into ringbuffer
     * via tcp_input() for registered connections, or into q_tcp for
     * unregistered ones (handshake phase). Check both paths. */
    while (total < bufsize) {
        if (timer_ms() - last_data_ms > tmo) break;

        /* Check ringbuffer first — tcp_input() deposits data here */
        int rn = rxring_pop(&c->rx, (uint8_t *)buf + total, bufsize - total);
        if (rn > 0) {
            total += rn;
            last_data_ms = timer_ms();
            continue;
        }

        /* Check FIN/RST set by tcp_input() while we were waiting */
        if (c->got_fin || c->got_rst) break;

        uint8_t reply[Q_PKT];
        int len = q_pop(&q_tcp, reply, sizeof(reply));
        if (len < 54) { net_idle(); continue; }

        /* Non-matching packets: re-queue */
        if (get16(reply + 36) != c->local_port ||
            get16(reply + 34) != c->remote_port) {
            q_push(&q_tcp, reply, len);
            net_idle();
            continue;
        }

        uint8_t flags = reply[47];
        int doff = (reply[46] >> 4) * 4;
        if (doff < 20) doff = 20;
        int ip_total = get16(reply + 16);
        if (ip_total < 20 + doff) continue;
        int plen = ip_total - 20 - doff;
        if (14 + 20 + doff + plen > len) plen = len - 14 - 20 - doff;
        if (plen < 0) continue;
        uint32_t tseq = get32(reply + 38);

        /* RST */
        if (flags & 0x04) {
            c->state = TCP_CLOSED;
            c->got_rst = 1;
            break;
        }

        if (plen > 0) {
            if (tseq != c->rcv_nxt) { send_tcp(c, 0x10, 0, 0); continue; }
            c->rcv_nxt = tseq + (uint32_t)plen;
            uint8_t *payload = reply + 14 + 20 + doff;

            int cp = plen;
            if (total + cp > bufsize) cp = bufsize - total;
            mcpy((uint8_t *)buf + total, payload, cp);
            total += cp;

            /* Overflow → push to ringbuffer */
            if (cp < plen)
                rxring_push(&c->rx, payload + cp, plen - cp);

            send_tcp(c, 0x10, 0, 0);
            last_data_ms = timer_ms();
        }

        if (flags & 0x01) { /* FIN */
            if (plen == 0 && tseq != c->rcv_nxt) continue;
            c->got_fin = 1;
            c->rcv_nxt++;
            send_tcp(c, 0x11, 0, 0);
            c->state = TCP_CLOSE_WAIT;
            break;
        }
    }

    if (total == 0 && !c->got_fin && !c->got_rst) return -11; /* -EAGAIN */
    return total;
}

/* ── TCP Close ─────────────────────────────────────── */

void net_tcp_close(net_tcp_t *c) {
    if (c->state == TCP_ESTABLISHED) {
        send_tcp(c, 0x11, 0, 0);
        c->snd_nxt++;
        c->state = TCP_FIN_WAIT1;

        /* tcp_input() handles ACK/FIN and advances state directly */
        uint64_t deadline = timer_ms() + 2000;
        while (timer_ms() < deadline) {
            if (c->state >= TCP_TIME_WAIT || c->state == TCP_CLOSED) break;
            net_idle();
        }
        c->state = TCP_CLOSED;
    } else if (c->state == TCP_CLOSE_WAIT) {
        send_tcp(c, 0x11, 0, 0);
        c->snd_nxt++;
        c->state = TCP_LAST_ACK;

        uint64_t deadline = timer_ms() + 2000;
        while (timer_ms() < deadline) {
            if (c->state == TCP_CLOSED) break;
            net_idle();
        }
        c->state = TCP_CLOSED;
    }
    tcp_unregister(c);
    rxring_destroy(&c->rx);
    c->state = TCP_CLOSED;
}
