/* CosmoRT TCP over IPv6 — input path.
 *
 * Mirrors tcp_input from tcp.c but for IPv6 framing. The full state
 * machine, congestion control, OOO reassembly stay in tcp.c via
 * shared helpers (send_tcp, send_synack_req, tcp_req_*). This file is
 * deliberately narrow: parse + cksum + lookup + admit-to-listener +
 * deliver-to-established. */

#include "net/tcp6.h"
#include "net/tcp.h"
#include "net/in6.h"
#include "net/ipv6.h"
#include "net/socket.h"
#include "net/net.h"
#include "net/net_util.h"
#include "core/timer.h"
#include "core/waitqueue.h"
#include "mm/slab.h"
#include "proc/process.h"

/* Helpers exported by tcp.c (made non-static for IPv6 reuse). */
extern void           send_tcp(net_tcp_t *c, uint8_t flags, const void *data, int dlen);
extern void           send_synack_req(const tcp_request_t *r);
extern tcp_request_t *tcp_req_alloc(void);
extern void           tcp_req_free(tcp_request_t *r);
extern void           tcp_req_enqueue(tcp_request_t **head, tcp_request_t *r);
extern tcp_request_t *tcp_req_find(tcp_request_t *head,
                                   const uint8_t *src_ip, uint16_t src_port);
extern tcp_request_t *tcp_req_remove(tcp_request_t **head,
                                     const uint8_t *src_ip, uint16_t src_port);
extern void           tcp_req_prime_stub(net_tcp_t *stub, const tcp_request_t *r);

/* sock_listener_tcp variant for v6 listeners. */
extern socket_t      *sock_find_listener6(uint32_t ns_id, uint16_t local_port_host);

static net_tcp_t *sock_listener_tcp6(uint32_t ns_id, uint16_t lport_host) {
    socket_t *s = sock_find_listener6(ns_id, lport_host);
    return s ? &s->tcp : 0;
}

/* IPv6 TCP checksum — duplicate of static tcp_cksum6 in tcp.c so this
 * file links without exposing internal helpers. */
static uint16_t tcp_cksum6_local(const struct in6_addr *src,
                                 const struct in6_addr *dst,
                                 const uint8_t *tcp, int tlen) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)((src->s6_addr[i] << 8) | src->s6_addr[i + 1]);
    for (int i = 0; i < 16; i += 2)
        sum += (uint32_t)((dst->s6_addr[i] << 8) | dst->s6_addr[i + 1]);
    sum += (uint32_t)((tlen >> 16) & 0xFFFF);
    sum += (uint32_t)(tlen & 0xFFFF);
    sum += 6;
    for (int i = 0; i < tlen; i += 2) {
        uint16_t w = (uint16_t)(tcp[i] << 8);
        if (i + 1 < tlen) w |= tcp[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Synthesize a tcp_request_t for an incoming v6 SYN. Mirrors the v4
 * SYN-handling block in tcp_input but works on parsed IPv6. */
static void admit_v6_syn(uint32_t ns_id, const ipv6_pkt_t *p,
                         net_tcp_t *ltcp,
                         uint16_t sport, uint16_t dport,
                         uint32_t in_seq, uint8_t in_flags,
                         int in_doff, const uint8_t *opts, int optlen)
{
    (void)ns_id;
    /* Retransmitted SYN for an in-flight request: re-emit SYN-ACK. */
    for (tcp_request_t *r = ltcp->syn_queue; r; r = r->next)
        if (r->is_v6 && in6_eq(&r->src_ip6, &p->src) && r->src_port == sport) {
            send_synack_req(r);
            return;
        }

    uint32_t cap = ltcp->backlog ? ltcp->backlog : TCP_SOMAXCONN_DEFAULT;
    if (ltcp->syn_qlen + ltcp->accept_qlen >= cap) return;

    tcp_request_t *r = tcp_req_alloc();
    if (!r) return;

    r->is_v6 = 1;
    in6_copy(&r->src_ip6,   &p->src);
    in6_copy(&r->local_ip6, &p->dst);
    /* MAC from ethernet src. */
    for (int i = 0; i < 6; i++) r->src_mac[i] = p->frame[6 + i];
    r->src_port   = sport;
    r->local_port = dport;
    r->irs        = in_seq;
    uint32_t our_iss;
    net_random(&our_iss, (int)sizeof(our_iss));
    r->iss = our_iss;

    /* Mirror peer SYN options (Window-Scale, Timestamps, ECN flag). */
    int i = 0;
    while (i < optlen) {
        uint8_t kind = opts[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= optlen) break;
        uint8_t olen = opts[i + 1];
        if (olen < 2 || i + olen > optlen) break;
        if (kind == 3 && olen == 3) {
            r->snd_wscale = opts[i + 2];
            if (r->snd_wscale > 14) r->snd_wscale = 14;
            r->wscale_ok = 1;
        } else if (kind == 8 && olen == 10) {
            r->ts_enabled = 1;
            r->ts_recent  = get32(opts + i + 2);
        }
        i += olen;
    }
    /* ECN negotiation flag on SYN: CWR+ECE both set. */
    if ((in_flags & 0xC0) == 0xC0) r->ecn_enabled = 1;

    tcp_req_enqueue(&ltcp->syn_queue, r);
    ltcp->syn_qlen++;

    send_synack_req(r);

    /* Wake epoll on the listener for accept(). */
    extern void epoll_wake_all(void);
    epoll_wake_all();

    (void)in_doff; (void)dport;
}

/* Promote a half-open request after the 3WHS-completing ACK, optionally
 * buffer payload. Mirrors the ACK branch of v4 tcp_input. */
static void promote_v6_request(net_tcp_t *ltcp,
                               const ipv6_pkt_t *p,
                               uint16_t sport,
                               uint32_t in_seq, uint32_t ack_num,
                               int plen, const uint8_t *payload)
{
    /* Walk syn_queue for a v6 entry matching {src_ip6, src_port}. */
    tcp_request_t **pp = &ltcp->syn_queue;
    tcp_request_t *r = 0;
    while (*pp) {
        if ((*pp)->is_v6 && in6_eq(&(*pp)->src_ip6, &p->src) &&
            (*pp)->src_port == sport) {
            r = *pp; *pp = r->next; r->next = 0; break;
        }
        pp = &(*pp)->next;
    }
    if (r) {
        if (ack_num != r->iss + 1) { tcp_req_free(r); return; }
        if (ltcp->syn_qlen > 0) ltcp->syn_qlen--;
        r->iss = ack_num;
        if (plen > 0 && in_seq == r->irs + 1) {
            int n = plen > TCP_REQ_EARLY_DATA ? TCP_REQ_EARLY_DATA : plen;
            mcpy(r->early_data, payload, n);
            r->early_len = (uint16_t)n;
            r->irs = r->irs + (uint32_t)n;
        }
        tcp_req_enqueue(&ltcp->accept_queue, r);
        ltcp->accept_qlen++;

        wake_up(&ltcp->wait_wq);
        extern void epoll_wake_all(void);
        epoll_wake_all();
        return;
    }
    /* Not in syn_queue — search accept_queue for early data. */
    for (tcp_request_t *q = ltcp->accept_queue; q; q = q->next) {
        if (q->is_v6 && in6_eq(&q->src_ip6, &p->src) &&
            q->src_port == sport) {
            if (plen > 0) {
                uint32_t expected = q->irs + 1;
                if (in_seq == expected) {
                    int free_space = TCP_REQ_EARLY_DATA - q->early_len;
                    int n = plen > free_space ? free_space : plen;
                    if (n > 0) {
                        mcpy(q->early_data + q->early_len, payload, n);
                        q->early_len = (uint16_t)(q->early_len + n);
                        q->irs = q->irs + (uint32_t)n;
                    }
                }
                net_tcp_t stub;
                tcp_req_prime_stub(&stub, q);
                send_tcp(&stub, 0x10, 0, 0);
            }
            return;
        }
    }
}

/* Process a TCP6 segment for an existing connection — handles SYN-ACK
 * (connect completion), data + ACK + FIN. Reuses send_tcp() from tcp.c
 * for ACK emission. */
static void process_v6_segment(net_tcp_t *c, const uint8_t *t, int tlen,
                               int doff, uint8_t flags, uint32_t tseq)
{
    int plen = tlen - doff;
    if (plen < 0) plen = 0;
    const uint8_t *payload = t + doff;

    /* SYN-ACK for outgoing connect. */
    if (c->state == TCP_SYN_SENT && (flags & 0x12) == 0x12) {
        c->rcv_nxt = tseq + 1;
        c->snd_nxt++;
        c->snd_una = c->snd_nxt;
        uint32_t raw_wnd = get16(t + 14);
        c->snd_wnd = raw_wnd;
        send_tcp(c, 0x10, 0, 0);
        c->state = TCP_ESTABLISHED;
        c->connect_err = 0;
        wake_up(&c->wait_wq);
        return;
    }

    /* RST. */
    if (flags & 0x04) {
        c->state = TCP_CLOSED;
        c->got_rst = 1;
        c->connect_err = -1;
        wake_up_all(&c->wait_wq);
        return;
    }

    /* ACK processing. */
    if (flags & 0x10) {
        uint32_t ack_num = get32(t + 8);
        if ((int32_t)(ack_num - c->snd_una) > 0) c->snd_una = ack_num;
        c->snd_wnd = get16(t + 14);
        if (c->state == TCP_FIN_WAIT1 && ack_num == c->snd_nxt)
            c->state = TCP_FIN_WAIT2;
        else if (c->state == TCP_LAST_ACK && ack_num == c->snd_nxt) {
            c->state = TCP_CLOSED;
            tcp_unregister(c);
        }
    }

    /* Data. */
    if (plen > 0 && tseq == c->rcv_nxt) {
        int stored = rxring_push(&c->rx, payload, plen);
        c->rcv_nxt = tseq + (uint32_t)stored;
        send_tcp(c, 0x10, 0, 0);
        wake_up(&c->wait_wq);
    }

    /* FIN. */
    if (flags & 0x01) {
        uint32_t fin_seq = tseq + (uint32_t)plen;
        if (fin_seq != c->rcv_nxt) return;
        c->got_fin = 1;
        c->rcv_nxt++;
        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_CLOSE_WAIT;
            send_tcp(c, 0x10, 0, 0);
        } else if (c->state == TCP_FIN_WAIT1) {
            c->state = TCP_CLOSING;
            send_tcp(c, 0x10, 0, 0);
        } else if (c->state == TCP_FIN_WAIT2) {
            c->state = TCP_TIME_WAIT;
            send_tcp(c, 0x10, 0, 0);
        }
        wake_up_all(&c->wait_wq);
    }
}

/* ── Main entry from ipv6_input ───────────────────── */

void tcp6_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    if (p->l4_len < 20) return;
    const uint8_t *t = p->frame + p->l4_off;

    /* Verify pseudo-header checksum. */
    uint8_t copy[1500];
    int n = p->l4_len > (int)sizeof(copy) ? (int)sizeof(copy) : p->l4_len;
    for (int i = 0; i < n; i++) copy[i] = t[i];
    uint16_t got = (uint16_t)((copy[16] << 8) | copy[17]);
    copy[16] = copy[17] = 0;
    uint16_t want = tcp_cksum6_local(&p->src, &p->dst, copy, n);
    if (got != want) return;

    uint16_t sport = get16(t);
    uint16_t dport = get16(t + 2);
    uint32_t tseq  = get32(t + 4);
    int doff       = (t[12] >> 4) * 4;
    if (doff < 20) doff = 20;
    if (doff > n)  doff = n;
    uint8_t flags  = t[13];

    /* Existing connection? */
    net_tcp_t *c = tcp_find6(ns_id, dport, sport, &p->src);
    if (c) {
        process_v6_segment(c, t, n, doff, flags, tseq);
        return;
    }

    /* No matching connection — see if a v6 listener exists. */
    net_tcp_t *ltcp = sock_listener_tcp6(ns_id, dport);
    if ((flags & 0x3F) == 0x02 && ltcp) {
        admit_v6_syn(ns_id, p, ltcp, sport, dport, tseq, flags,
                     doff, t + 20, doff - 20);
        return;
    }
    if ((flags & 0x10) && ltcp) {
        int plen = n - doff;
        if (plen < 0) plen = 0;
        promote_v6_request(ltcp, p, sport, tseq, get32(t + 8),
                           plen, t + doff);
        return;
    }
    /* SYN to closed v6 port — silent drop (no RST6 emit yet). */
}
