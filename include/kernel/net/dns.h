/* CosmoRT DNS Resolver — kernel-level DNS for boot */
#ifndef DNS_H
#define DNS_H

#include <stdint.h>

int net_dns_resolve(const char *hostname, uint8_t ip_out[4]);

extern uint16_t dns_local_port;

#endif
