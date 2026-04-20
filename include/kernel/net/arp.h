/* CosmoRT ARP — Hash-Table Cache + Resolution
 * Linux-Stil: NUD-States, Pending-Queue pro neighbour, TTL-GC.
 * Busy-Wait eliminiert: Miss schickt Request und queued das Paket; der
 * Reply-Handler flusht die Queue. */
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "config.h"

/* ── ARP Cache Config ─────────────────────────────── */

#define ARP_HASH_SIZE          64   /* buckets, must be power of 2    */
/* ARP cache entries are dynamically slab-allocated (no systemwide cap). */
#define ARP_PENDING_PER_ENTRY   3   /* queued frames per NUD_INCOMPLETE */
#define ARP_PENDING_PKT_SIZE 1536   /* max frame size                 */
#define ARP_INCOMPLETE_TTL_MS 30000 /* drop stuck INCOMPLETE entries  */
#define ARP_REACHABLE_TTL_MS  (10 * 60 * 1000) /* 10 min REACHABLE    */

/* ── Neighbour States (RFC 4861 §7.3, Linux include/net/neighbour.h) ─ */

enum arp_nud {
    NUD_NONE       = 0,
    NUD_INCOMPLETE = 1,
    NUD_REACHABLE  = 2,
    NUD_FAILED     = 3,
};

/* ── Pending Frame ────────────────────────────────── */

typedef struct arp_pending {
    uint16_t len;
    uint8_t  data[ARP_PENDING_PKT_SIZE];
} arp_pending_t;

/* ── ARP Entry ────────────────────────────────────── */

typedef struct arp_entry {
    uint32_t           ip_key;     /* IP as uint32 for fast compare  */
    uint8_t            mac[6];
    uint8_t            state;      /* enum arp_nud                   */
    uint8_t            _pad;
    uint64_t           created_ms; /* for TTL / INCOMPLETE timeout   */
    uint64_t           last_used_ms;
    struct arp_entry  *hash_next;  /* chain within bucket            */
    int                pending_count;
    arp_pending_t      pending[ARP_PENDING_PER_ENTRY];
} arp_entry_t;

/* ── Public API ───────────────────────────────────── */

/* Resolve IP → MAC. Cache-hit REACHABLE: fills mac_out, returns 0.
 * Cache-miss: sends ARP request, brief bounded poll (250ms) so existing
 * synchronous callers still work for one-shot resolution. Returns
 * -EHOSTUNREACH on timeout (Linux-konsistent). */
int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

/* Non-blocking variant: cache-hit -> 0, miss -> queue the frame on the
 * neighbour, send ARP request, return 0. Returns -EHOSTUNREACH only when
 * queue is full or neighbour already NUD_FAILED.  Caller must NOT retry
 * the same frame; the ARP reply handler will send it. */
int net_arp_queue_frame(const uint8_t *ip, const uint8_t *frame, int len);

/* Process incoming ARP packet (full Ethernet frame).
 * Handles ARP replies (updates cache + flushes pending) and requests. */
void arp_input(const uint8_t *pkt, int len);

/* Periodic garbage collection: drop stuck INCOMPLETE entries, age
 * REACHABLE entries.  Call from tick/net_poll. */
void arp_cache_gc(void);

/* Cache lookup: returns 0 if found in NUD_REACHABLE, -1 otherwise. */
int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out);

/* Cache insert/update. */
void arp_cache_insert(const uint8_t *ip, const uint8_t *mac);

/* Cache evict entry for IP. */
void arp_cache_evict(const uint8_t *ip);

/* Reset cache (for testing). */
void arp_cache_reset(void);

#endif
