/* CosmoRT UDP over IPv6 — RFC 768 + 8200 §8.1 pseudo-header.
 *
 * Per-NS port→queue hash. Reuses the v4 udp_sock_t struct (with is_v6
 * flag); the bind/send/recv API is parallel to the v4 path. */
#ifndef COSMO_NET_UDP6_H
#define COSMO_NET_UDP6_H

/* net.h is the canonical entry that defines pkt_queue_t and pulls in
 * net/udp.h which uses it. */
#include "net/net.h"
#include "net/in6.h"
#include "net/ipv6.h"

void udp6_init(void);

/* Send via IPv6. Returns -1 on neighbour-resolve failure or MTU exceeded. */
int udp6_send(uint32_t ns_id, const struct in6_addr *dst, uint16_t dst_port,
              uint16_t src_port, const void *data, int len);

/* Receive — pulls one datagram from the bound socket queue. */
int udp6_recv(uint32_t ns_id, uint16_t local_port,
              void *buf, int bufsize,
              struct in6_addr *src_out, uint16_t *src_port_out,
              int timeout_ms);

/* Demux — called by ipv6_input with parsed packet. */
int udp6_input(uint32_t ns_id, const ipv6_pkt_t *p);

udp_sock_t *udp6_bind_ns(uint32_t ns_id, uint16_t port);
udp_sock_t *udp6_find_ns(uint32_t ns_id, uint16_t port);
void        udp6_unbind (udp_sock_t *s);

#endif
