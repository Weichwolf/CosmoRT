/* CosmoRT TCP — Per-Socket Ringbuffer, State-Machine (RFC 793)
 * Congestion: CUBIC (RFC 8312), SACK (RFC 2018), Window Scaling (RFC 7323)
 * Fast Retransmit/Recovery (RFC 5681 §3.2), IW=10 (RFC 6928)
 * Timestamps/PAWS (RFC 7323), ECN (RFC 3168), TFO (RFC 7413) */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"
#include "net/in6.h"
#include "core/waitqueue.h"

/* ── Config ────────────────────────────────────────── */

/* TCP connections themselves are sock_slab-allocated (dynamic, on-demand).
 * Per-connection limits below are per-socket caps, not system-wide pools. */
#define NET_TCP_OOO_MAX   64   /* per-connection cap on OOO segments (DoS guard) */
#define NET_TFO_CACHE_MAX 64   /* TFO-cookie cache (global, server-IP keyed) */

/* ── TCP States (RFC 793) ─────────────────────────── */

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

/* ── Listen-backlog entry (half-open request_sock, RFC 793 §3.4) ─────
 * One per incoming SYN matched to a listening socket. Retains just
 * enough state to complete the 3-way handshake and graduate into a
 * full net_tcp_t when accept() runs. Fields mirror the minimum Linux
 * struct inet_request_sock / tcp_request_sock needs. */
/* Early data buffered on the half-open / accept_queue between the 3WHS
 * ACK and accept() consuming the request. Sized to hold one MSS so a
 * piggybacked "hi" or similar single-segment payload survives. Larger
 * bursts are dropped (peer will retransmit). */
#define TCP_REQ_EARLY_DATA 1460

typedef struct tcp_request {
    struct tcp_request *next;     /* singly-linked FIFO per queue */
    uint8_t  is_v6;
    uint8_t  _pad_v6;
    uint8_t  src_ip[4];           /* used when is_v6==0 */
    struct in6_addr src_ip6;      /* used when is_v6==1 */
    struct in6_addr local_ip6;    /* listener's address for v6 */
    uint8_t  src_mac[6];
    uint16_t src_port;            /* peer port (big-endian on wire, host here) */
    uint16_t local_port;          /* listener port (host order) */
    uint32_t iss;                 /* initial send seq (our SYN-ACK) */
    uint32_t irs;                 /* initial recv seq (peer's SYN) */
    /* Negotiated options from peer SYN — replayed into child net_tcp_t */
    uint8_t  wscale_ok;
    uint8_t  snd_wscale;
    uint8_t  ts_enabled;
    uint8_t  ecn_enabled;
    uint32_t ts_recent;
    /* Early-data buffer: payload on the handshake-completing ACK and
     * any segment arriving before accept() materialises the child. */
    uint16_t early_len;
    uint8_t  early_data[TCP_REQ_EARLY_DATA];
} tcp_request_t;

/* Per-listener backlog cap applied when user backlog exceeds Linux's
 * somaxconn. Matches Linux default /proc/sys/net/core/somaxconn. */
#define TCP_SOMAXCONN_DEFAULT 4096

/* ── Per-Socket RX Ringbuffer ─────────────────────── */

typedef struct {
    uint8_t  *buf;                /* allocated: NET_TCP_RXBUF bytes (power of 2) */
    uint32_t head, tail;          /* byte positions (mod NET_TCP_RXBUF) */
    spinlock_t lock;
} tcp_rxring_t;

/* Ringbuffer operations (defined in tcp.c) */
int      rxring_init(tcp_rxring_t *r);   /* allocates buffer, returns 0 or -ENOMEM */
void     rxring_destroy(tcp_rxring_t *r);
int      rxring_push(tcp_rxring_t *r, const void *data, int len);
int      rxring_pop(tcp_rxring_t *r, void *buf, int len);
int      rxring_used(const tcp_rxring_t *r);
int      rxring_free(const tcp_rxring_t *r);

/* ── TCP Connection ───────────────────────────────── */

typedef struct net_tcp {
    uint8_t  is_v6;             /* 1 = IPv6 connection (uses dst_ip6/local_ip6) */
    uint8_t  _pad_v6;
    uint8_t  dst_ip[4];
    uint8_t  dst_mac[6];
    uint16_t local_port, remote_port;
    /* IPv6-only addresses (zero on v4 sockets). */
    struct in6_addr dst_ip6;
    struct in6_addr local_ip6;

    uint32_t snd_nxt, snd_una;   /* send sequence space */
    uint32_t rcv_nxt;            /* receive sequence space */
    uint32_t snd_wnd, rcv_wnd;   /* flow control (scaled) */

    enum tcp_state state;

    tcp_rxring_t rx;

    /* Out-of-order queue: sorted singly-linked list of slab-allocated
     * tcp_ooo_seg_t (definition private to tcp.c). Linux uses an rb_tree
     * (out_of_order_queue); a sorted list suffices because TCP OOO depth
     * is typically small and bounded by NET_TCP_OOO_MAX per connection. */
    void *ooo_head;
    int   ooo_count;

    /* CUBIC Congestion Control (RFC 8312) */
    uint32_t cwnd;           /* congestion window (bytes) */
    uint32_t ssthresh;       /* slow-start threshold (bytes) */
    uint16_t dup_ack_count;  /* consecutive duplicate ACKs */
    uint8_t  in_recovery;    /* 1 = in Fast Recovery */
    uint32_t recovery_seq;   /* snd_nxt at entry to Fast Recovery */
    uint64_t cubic_t_epoch;  /* time of last loss event (ms) */
    uint32_t cubic_w_max;    /* cwnd at last loss */
    uint32_t cubic_w_last;   /* last cwnd before reduction */
    uint32_t cubic_k;        /* time to reach w_max (scaled ×1024) */

    /* SACK (RFC 2018) */
    struct { uint32_t left, right; } sack_blocks[4];
    int      sack_count;

    /* Window Scaling (RFC 7323) */
    uint8_t  snd_wscale;     /* peer's shift count */
    uint8_t  rcv_wscale;     /* our shift count (advertised) */
    uint8_t  wscale_ok;      /* 1 = both sides negotiated */

    /* Retransmit */
    uint64_t rto_ms, last_send_ms;

    uint8_t got_fin, got_rst;

    /* Per-socket timeouts (0 = use global default) */
    uint64_t rcv_timeo_ms;
    uint64_t snd_timeo_ms;

    /* Keepalive */
    uint8_t  keepalive;       /* 1 = enabled */
    uint8_t  keepalive_probes;/* failed probes so far */
    uint64_t keepalive_next;  /* next probe time (ms) */

    /* Non-blocking connect state */
    int8_t   connect_err;     /* 0=ok, <0=error code from async connect */

    /* Timestamps (RFC 7323) */
    uint8_t  ts_enabled;      /* 1 = peer negotiated timestamps */
    uint32_t ts_recent;       /* most recent TSval from peer */
    uint64_t ts_recent_age;   /* timer_ms() when ts_recent was set */

    /* ECN (RFC 3168) */
    uint8_t  ecn_enabled;     /* 1 = ECN negotiated */
    uint8_t  ecn_ce_pending;  /* 1 = received CE-marked packet, send ECE */
    uint8_t  ecn_cwr_sent;    /* 1 = CWR sent, awaiting ECE clear */

    /* TFO (RFC 7413) */
    uint8_t  tfo_enabled;     /* 1 = TFO cookie stored for this server */
    uint8_t  tfo_cookie[16];  /* cached cookie */
    uint8_t  tfo_cookie_len;  /* 0 = no cookie */

    /* Per-connection waitqueue. tcp_input wakes here on:
     *   - SYN-ACK arrival (connect completion)
     *   - data push into rx ringbuffer
     *   - RST / FIN / state transition
     * Listening sockets reuse the same wq for accept_queue admission
     * (a net_tcp_t is either listener or connection, never both at once). */
    wait_queue_head_t wait_wq;

    /* Owning network-namespace ID (init_net_ns.ns_id == 1). Hash key
     * includes this so two NS can hold the same 4-tuple independently. */
    uint32_t ns_id;

    /* Hash-table chaining (tcp_hash bucket linked list) */
    struct net_tcp *hash_next;

    /* Listen-backlog (RFC 793 "passive open" queues).
     * Populated only on listening sockets. See tcp_req_* helpers in tcp.c.
     * syn_queue:    half-open state after SYN → SYN-ACK sent, awaiting ACK.
     * accept_queue: handshake complete, ready for accept() to consume.
     * backlog:      applied cap (Linux: min(somaxconn, listen()'s backlog+1)). */
    struct tcp_request *syn_queue;
    struct tcp_request *accept_queue;
    uint32_t syn_qlen, accept_qlen;
    uint32_t backlog;
} net_tcp_t;

/* ── TFO Cookie Cache (RFC 7413) ─────────────────── */

typedef struct {
    uint8_t  ip[4];
    uint8_t  cookie[16];
    uint8_t  cookie_len;    /* 4-16, 0 = empty slot */
} tfo_cache_entry_t;

int  tfo_cache_lookup(const uint8_t *ip, uint8_t *cookie_out, uint8_t *len_out);
void tfo_cache_store(const uint8_t *ip, const uint8_t *cookie, uint8_t len);

/* ── TCP Hash Table ───────────────────────────────── */

/* Register/unregister connection in hash table for O(1) lookup. The
 * connection's ns_id field is part of the lookup key. */
void     tcp_register(net_tcp_t *c);
void     tcp_unregister(net_tcp_t *c);
/* Lookup keyed by ns_id (caller's NS). */
net_tcp_t *tcp_find(uint32_t ns_id, uint16_t local_port, uint16_t remote_port,
                    const uint8_t *src_ip);
/* IPv6 lookup variant — distinguished by is_v6 bit on the connection. */
net_tcp_t *tcp_find6(uint32_t ns_id, uint16_t local_port, uint16_t remote_port,
                     const struct in6_addr *src_ip);

/* ── TCP API (called by socket.c) ─────────────────── */

int  net_tcp_connect(net_tcp_t *c, const uint8_t *dst_ip, uint16_t port);
int  net_tcp6_connect(net_tcp_t *c, const struct in6_addr *dst, uint16_t port);
int  net_tcp_accept(net_tcp_t *c, uint16_t local_port, int timeout_ms);
int  net_tcp_accept_child(net_tcp_t *listener, net_tcp_t *child);
int  net_tcp_send(net_tcp_t *c, const void *data, int len);
int  net_tcp_recv(net_tcp_t *c, void *buf, int bufsize, int timeout_ms);
void net_tcp_close(net_tcp_t *c);

/* Listen-backlog management — called by socket.c close/shutdown paths.
 * Frees every pending tcp_request_t on both syn_queue and accept_queue. */
void tcp_listener_drain(net_tcp_t *c);

/* Pop one completed half-open into *out_req (caller owns it and must
 * tcp_req_release() it once the state has been cloned into the new
 * net_tcp_t). Returns 1 on success, 0 on empty. Non-blocking. */
struct tcp_request;
int  tcp_listener_pop_accept(net_tcp_t *c, struct tcp_request **out_req);
void tcp_req_release(struct tcp_request *r);

/* ── TCP Input (called by net.c dispatcher / loopback) ─────────────
 * ns_id selects which NS hosts the listening socket / connection.
 * Wire packets from physical NICs always belong to init_net_ns; loopback
 * carries the sender's NS so per-NS lo isolation is enforced. */

void tcp_input(uint32_t ns_id, const uint8_t *pkt, int len);

/* ── Internals shared with net.c ──────────────────── */

/* Helpers from net.c used by tcp.c */
void net_send_raw(const uint8_t *data, uint16_t len);
void net_build_ip_hdr(uint8_t *pkt, const uint8_t *dst_mac,
                      const uint8_t *dst_ip, uint8_t proto, uint16_t plen);
void net_build_ip_hdr_tos(uint8_t *pkt, const uint8_t *dst_mac,
                          const uint8_t *dst_ip, uint8_t proto,
                          uint16_t plen, uint8_t tos);
int  net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

#endif
