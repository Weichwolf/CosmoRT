/* CosmoRT ARP — Cache + Resolution, extracted from net.c (Phase C) */
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "config.h"

/* ARP cache size */
#define NET_ARP_CACHE 16

/* Resolve IP → MAC via ARP request/reply. Blocks until reply or timeout.
 * Returns 0 on success, -1 on timeout. */
int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

/* Process incoming ARP packet (full Ethernet frame).
 * Handles ARP replies (updates cache) and ARP requests (sends reply). */
void arp_input(const uint8_t *pkt, int len);

/* Cache lookup: returns 0 if found, -1 if miss. */
int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out);

/* Cache insert/update. */
void arp_cache_insert(const uint8_t *ip, const uint8_t *mac);

/* Cache evict entry for IP. */
void arp_cache_evict(const uint8_t *ip);

#endif
