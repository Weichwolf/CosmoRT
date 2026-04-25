/* CosmoRT TCP over IPv6 — input + small helpers.
 *
 * The egress path lives in tcp.c (`send_tcp_opts` knows is_v6). The
 * full state machine, congestion control, OOO reassembly are also in
 * tcp.c — tcp6_input handles the IPv6-specific parts (header parse,
 * checksum, NS-keyed v6 hash lookup, listener admission) and then
 * either materialises a tcp_request_t or hands the segment to a
 * shared post-lookup helper. */
#ifndef COSMO_NET_TCP6_H
#define COSMO_NET_TCP6_H

#include "net/in6.h"
#include "net/ipv6.h"

void tcp6_input(uint32_t ns_id, const ipv6_pkt_t *p);

#endif
