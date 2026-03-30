/* CosmoRT UDP — Hash-Table Demux, Slab-backed Socket Pool */
#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

struct thread;

#define UDP_POOL_SIZE 128
#define UDP_HASH_SIZE  64

typedef struct udp_sock {
    uint16_t   port;
    pkt_queue_t q;
    struct thread *wait_thread;
    struct udp_sock *hash_next;
} udp_sock_t;

udp_sock_t *udp_bind(uint16_t port);

void udp_unbind(udp_sock_t *s);

udp_sock_t *udp_find(uint16_t port);

int udp_input(const uint8_t *pkt, int len);

int net_udp_send(const uint8_t *dst_ip, uint16_t dst_port,
                 uint16_t src_port, const void *data, int len);
int net_udp_recv(uint16_t local_port, void *buf, int bufsize,
                 uint8_t *src_ip_out, uint16_t *src_port_out,
                 int timeout_ms);

int udp_poll_ready(uint16_t port);

#endif
