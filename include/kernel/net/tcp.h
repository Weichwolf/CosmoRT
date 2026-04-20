/* CosmoRT TCP — Per-Socket Ringbuffer, State-Machine (RFC 793)
 * Congestion: CUBIC (RFC 8312), SACK (RFC 2018), Window Scaling (RFC 7323)
 * Fast Retransmit/Recovery (RFC 5681 §3.2), IW=10 (RFC 6928)
 * Timestamps/PAWS (RFC 7323), ECN (RFC 3168), TFO (RFC 7413) */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

struct thread; /* forward declaration for wait_thread */

/* ── Config ────────────────────────────────────────── */

/* TCP connections themselves are sock_slab-allocated (dynamic, on-demand).
 * Per-connection limits below are per-socket caps, not system-wide pools. */
#define NET_TCP_OOO_SLOTS 4    /* out-of-order segments per connection */
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
    uint8_t  dst_ip[4];
    uint8_t  dst_mac[6];
    uint16_t local_port, remote_port;

    uint32_t snd_nxt, snd_una;   /* send sequence space */
    uint32_t rcv_nxt;            /* receive sequence space */
    uint32_t snd_wnd, rcv_wnd;   /* flow control (scaled) */

    enum tcp_state state;

    tcp_rxring_t rx;

    /* Out-of-order buffer (Phase E) */
    struct {
        uint32_t seq;
        uint16_t len;
        uint8_t  data[1460]; /* segment payload copy */
    } ooo[NET_TCP_OOO_SLOTS];
    int ooo_count;

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

    /* Sleep/wake: thread blocked on this connection (recv/connect/close) */
    struct thread *wait_thread;

    /* Hash-table chaining (tcp_hash bucket linked list) */
    struct net_tcp *hash_next;
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

/* Register/unregister connection in hash table for O(1) lookup */
void     tcp_register(net_tcp_t *c);
void     tcp_unregister(net_tcp_t *c);
net_tcp_t *tcp_find(uint16_t local_port, uint16_t remote_port, const uint8_t *src_ip);

/* ── TCP API (called by socket.c) ─────────────────── */

int  net_tcp_connect(net_tcp_t *c, const uint8_t *dst_ip, uint16_t port);
int  net_tcp_accept(net_tcp_t *c, uint16_t local_port, int timeout_ms);
int  net_tcp_send(net_tcp_t *c, const void *data, int len);
int  net_tcp_recv(net_tcp_t *c, void *buf, int bufsize, int timeout_ms);
void net_tcp_close(net_tcp_t *c);

/* ── TCP Input (called by net.c dispatcher) ───────── */

void tcp_input(const uint8_t *pkt, int len);

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
