/* CosmoRT TCP — Per-Socket Ringbuffer, State-Machine (RFC 793) */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

/* ── Config ────────────────────────────────────────── */

#define NET_TCP_MAX       32
#define NET_TCP_OOO_SLOTS 4

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
    uint16_t snd_wnd, rcv_wnd;   /* flow control */

    enum tcp_state state;

    tcp_rxring_t rx;

    /* Out-of-order buffer (Phase E) */
    struct { uint32_t seq; uint16_t len; uint16_t off; } ooo[NET_TCP_OOO_SLOTS];
    int ooo_count;

    /* Retransmit */
    uint64_t rto_ms, last_send_ms;

    uint8_t got_fin, got_rst;
} net_tcp_t;

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
int  net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

#endif
