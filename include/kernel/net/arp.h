/* CosmoRT ARP — Hash-Table Cache + Resolution */
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "config.h"

#define ARP_POOL_SIZE  128
#define ARP_HASH_SIZE   64

typedef struct arp_entry {
    uint32_t           ip_key;
    uint8_t            mac[6];
    uint8_t            valid;
    uint8_t            _pad;
    struct arp_entry  *hash_next;
} arp_entry_t;

int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

void arp_input(const uint8_t *pkt, int len);

int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out);

void arp_cache_insert(const uint8_t *ip, const uint8_t *mac);

void arp_cache_evict(const uint8_t *ip);

void arp_cache_reset(void);

#endif
