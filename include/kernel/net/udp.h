/* CosmoRT UDP — Per-Socket Demux, extracted from net.c (Phase B) */
#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

struct thread; /* forward declaration for wait_thread */

/* ── Config ────────────────────────────────────────── */

#define NET_UDP_MAX 16

/* ── Per-Socket UDP Queue ──────────────────────────── */

typedef struct {
    uint16_t   port;       /* host byte order, 0 = unused */
    pkt_queue_t q;
    struct thread *wait_thread;  /* thread blocked on recv, or NULL */
} udp_sock_t;

/* ── UDP Registration ──────────────────────────────── */

/* Bind a port for receiving. Returns udp_sock_t* or NULL if full. */
udp_sock_t *udp_bind(uint16_t port);

/* Unbind — frees the slot. */
void udp_unbind(udp_sock_t *s);

/* Find registered socket by port. Returns NULL if not bound. */
udp_sock_t *udp_find(uint16_t port);

/* ── UDP Input (called by net.c dispatcher) ────────── */

/* Dispatch incoming UDP packet to correct per-socket queue.
 * pkt is full Ethernet frame. Returns 1 if dispatched, 0 if no socket. */
int udp_input(const uint8_t *pkt, int len);

/* ── UDP API (called by socket.c) ──────────────────── */

int net_udp_send(const uint8_t *dst_ip, uint16_t dst_port,
                 uint16_t src_port, const void *data, int len);
int net_udp_recv(uint16_t local_port, void *buf, int bufsize,
                 uint8_t *src_ip_out, uint16_t *src_port_out,
                 int timeout_ms);

/* Check if a UDP socket with given port has data ready (for poll/epoll) */
int udp_poll_ready(uint16_t port);

#endif
