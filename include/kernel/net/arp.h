/* CosmoRT ARP — Hash-Table Cache + Resolution
 * Phase C: extracted from net.c.  Skal-D: hash-table + slab pool. */
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "config.h"

/* ── ARP Cache Config ─────────────────────────────── */

#define ARP_POOL_SIZE  128         /* max cached entries              */
#define ARP_HASH_SIZE   64         /* buckets, must be power of 2    */

/* ── ARP Entry ────────────────────────────────────── */

typedef struct arp_entry {
    uint32_t           ip_key;     /* IP as uint32 for fast compare  */
    uint8_t            mac[6];
    uint8_t            valid;
    uint8_t            _pad;
    struct arp_entry  *hash_next;  /* chain within bucket            */
} arp_entry_t;

/* ── Public API ───────────────────────────────────── */

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

/* Reset cache (for testing). */
void arp_cache_reset(void);

#endif
