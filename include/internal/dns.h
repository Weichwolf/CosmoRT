/* CosmoRT DNS Resolver — kernel-level DNS for boot
 * Extracted from net.c (Phase D1).
 */
#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/* Resolve hostname to IPv4 address via UDP DNS query.
 * Returns 0 on success, -1 on failure/timeout. */
int net_dns_resolve(const char *hostname, uint8_t ip_out[4]);

/* DNS local port (set during active query, 0 otherwise).
 * Used by dispatch.c to route DNS replies. */
extern uint16_t dns_local_port;

#endif
