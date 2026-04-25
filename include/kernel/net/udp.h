/* CosmoRT UDP — Hash-Table Demux, Slab-backed Socket Pool */
#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

struct thread; /* forward declaration for wait_thread */

/* ── Config ────────────────────────────────────────── */

#define UDP_HASH_SIZE  64          /* buckets, must be power of 2      */
/* UDP sockets are dynamically slab-allocated (no systemwide cap).
 * Per-process bound is RLIMIT_NOFILE (one fd per socket). */

/* ── Per-Socket UDP Queue ──────────────────────────── */

typedef struct udp_sock {
    uint16_t   port;               /* host byte order, 0 = unused      */
    uint32_t   ns_id;              /* owning network namespace         */
    pkt_queue_t q;
    struct thread *wait_thread;    /* thread blocked on recv, or NULL   */
    struct udp_sock *hash_next;    /* hash-chain link                   */
} udp_sock_t;

/* ── UDP Init ──────────────────────────────────────── */

/* Pre-warm slab so first udp_bind sees a populated free list (avoids
 * timing-sensitive page_alloc on the recv path of the first DNS query). */
void udp_init(void);

/* ── UDP Registration ──────────────────────────────── */

/* Bind a port in the given NS. Returns udp_sock_t* or NULL if OOM. */
udp_sock_t *udp_bind_ns(uint32_t ns_id, uint16_t port);

/* Bind in current task's NS (back-compat). */
udp_sock_t *udp_bind(uint16_t port);

/* Unbind — frees the slot. */
void udp_unbind(udp_sock_t *s);

/* Find registered socket by (ns_id, port). Returns NULL if not bound. */
udp_sock_t *udp_find_ns(uint32_t ns_id, uint16_t port);

/* Find in current task's NS (back-compat). */
udp_sock_t *udp_find(uint16_t port);

/* ── UDP Input (called by net.c dispatcher) ────────── */

/* Dispatch incoming UDP packet to correct per-socket queue.
 * pkt is full Ethernet frame. Returns 1 if dispatched, 0 if no socket. */
int udp_input(uint32_t ns_id, const uint8_t *pkt, int len);

/* ── UDP API (called by socket.c) ──────────────────── */

int net_udp_send(const uint8_t *dst_ip, uint16_t dst_port,
                 uint16_t src_port, const void *data, int len);
int net_udp_recv(uint16_t local_port, void *buf, int bufsize,
                 uint8_t *src_ip_out, uint16_t *src_port_out,
                 int timeout_ms);

/* Check if a UDP socket with given port has data ready (for poll/epoll) */
int udp_poll_ready(uint16_t port);

#endif
