/* CosmoRT ARP — Hash-Table Cache + Resolution
 * Skal-D: O(1) amortised lookup via hash-table + slab pool.
 * Replaces linear array (Phase C).
 */

#include "net/arp.h"
#include "net/ip.h"
#include "net/net.h"
#include "net/net_util.h"
#include "core/timer.h"
#include "mm/slab.h"

/* ── Helpers ──────────────────────────────────────── */

static inline uint32_t ip_to_u32(const uint8_t *ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8)  |  (uint32_t)ip[3];
}

/* ── Slab Pool ────────────────────────────────────── */

static arp_entry_t arp_pool[ARP_POOL_SIZE];
static slab_t      arp_slab;
static int         arp_slab_inited;

static void arp_slab_ensure_init(void) {
    if (__builtin_expect(arp_slab_inited, 1)) return;
    slab_init(&arp_slab, arp_pool, (int)sizeof(arp_entry_t), ARP_POOL_SIZE);
    arp_slab_inited = 1;
}

/* ── Hash Table (IP → chain) ─────────────────────── */

static arp_entry_t *arp_hash[ARP_HASH_SIZE];
static int          arp_evict_bucket;  /* round-robin bucket for eviction */

static inline int bucket_idx(uint32_t key) {
    return (int)(key & (ARP_HASH_SIZE - 1));
}

/* ── Cache Reset ──────────────────────────────────── */

void arp_cache_reset(void) {
    for (int i = 0; i < ARP_HASH_SIZE; i++)
        arp_hash[i] = 0;
    arp_evict_bucket = 0;
    arp_slab_inited = 0;
    arp_slab_ensure_init();
}

/* ── Lookup: O(1) amortised ──────────────────────── */

int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out) {
    uint32_t key = ip_to_u32(ip);
    int idx = bucket_idx(key);
    for (arp_entry_t *e = arp_hash[idx]; e; e = e->hash_next) {
        if (e->ip_key == key) {
            mcpy(mac_out, e->mac, 6);
            return 0;
        }
    }
    return -1;
}

/* ── Evict one entry from a bucket (tail of chain) ── */

static void evict_one(void) {
    /* Round-robin across buckets to find a non-empty one */
    for (int tries = 0; tries < ARP_HASH_SIZE; tries++) {
        int b = arp_evict_bucket % ARP_HASH_SIZE;
        arp_evict_bucket = b + 1;
        arp_entry_t *e = arp_hash[b];
        if (!e) continue;

        /* Remove tail of chain (simplest LRU approximation) */
        if (!e->hash_next) {
            /* Single entry in bucket */
            arp_hash[b] = 0;
            slab_free(&arp_slab, e);
            return;
        }
        arp_entry_t *prev = e;
        while (prev->hash_next->hash_next)
            prev = prev->hash_next;
        slab_free(&arp_slab, prev->hash_next);
        prev->hash_next = 0;
        return;
    }
}

/* ── Insert / Update ─────────────────────────────── */

void arp_cache_insert(const uint8_t *ip, const uint8_t *mac) {
    arp_slab_ensure_init();

    uint32_t key = ip_to_u32(ip);
    int idx = bucket_idx(key);

    /* Update existing? */
    for (arp_entry_t *e = arp_hash[idx]; e; e = e->hash_next) {
        if (e->ip_key == key) {
            mcpy(e->mac, mac, 6);
            return;
        }
    }

    /* Allocate new entry */
    arp_entry_t *n = slab_alloc(&arp_slab);
    if (!n) {
        /* Pool full — evict one, retry */
        evict_one();
        n = slab_alloc(&arp_slab);
        if (!n) return;  /* should not happen */
    }
    n->ip_key = key;
    mcpy(n->mac, mac, 6);
    n->valid = 1;
    n->hash_next = arp_hash[idx];
    arp_hash[idx] = n;
}

/* ── Evict by IP ─────────────────────────────────── */

void arp_cache_evict(const uint8_t *ip) {
    arp_slab_ensure_init();

    uint32_t key = ip_to_u32(ip);
    int idx = bucket_idx(key);
    arp_entry_t *prev = 0;
    for (arp_entry_t *e = arp_hash[idx]; e; prev = e, e = e->hash_next) {
        if (e->ip_key == key) {
            if (prev) prev->hash_next = e->hash_next;
            else      arp_hash[idx] = e->hash_next;
            slab_free(&arp_slab, e);
            return;
        }
    }
}

/* ── ARP Input (from dispatcher) ──────────────────── */

void arp_input(const uint8_t *pkt, int len) {
    if (len < 42) return;
    uint16_t op = get16(pkt + 20);

    /* Extract sender MAC/IP from ARP header */
    const uint8_t *sender_mac = pkt + 22;
    const uint8_t *sender_ip  = pkt + 28;

    /* Filter bogus MACs: all-FF or all-00 */
    if ((sender_mac[0] == 0xFF && sender_mac[1] == 0xFF && sender_mac[2] == 0xFF &&
         sender_mac[3] == 0xFF && sender_mac[4] == 0xFF && sender_mac[5] == 0xFF) ||
        (sender_mac[0] == 0 && sender_mac[1] == 0 && sender_mac[2] == 0 &&
         sender_mac[3] == 0 && sender_mac[4] == 0 && sender_mac[5] == 0))
        return;

    if (op == 2) {
        /* ARP Reply — update cache */
        arp_cache_insert(sender_ip, sender_mac);
    } else if (op == 1) {
        /* ARP Request — if target IP is ours, send reply */
        const uint8_t *target_ip = pkt + 38;
        if (net_my_ip[0] && target_ip[0] == net_my_ip[0] &&
            target_ip[1] == net_my_ip[1] && target_ip[2] == net_my_ip[2] &&
            target_ip[3] == net_my_ip[3]) {
            /* Also learn the requester */
            arp_cache_insert(sender_ip, sender_mac);

            /* Build ARP reply */
            uint8_t reply[42];
            mzero(reply, 42);
            mcpy(reply, sender_mac, 6);         /* dst MAC */
            mcpy(reply + 6, net_my_mac, 6);     /* src MAC */
            put16(reply + 12, 0x0806);           /* ARP */
            put16(reply + 14, 1);                /* HW = Ethernet */
            put16(reply + 16, 0x0800);           /* Proto = IPv4 */
            reply[18] = 6; reply[19] = 4;       /* HW/Proto len */
            put16(reply + 20, 2);                /* op = reply */
            mcpy(reply + 22, net_my_mac, 6);    /* sender MAC */
            mcpy(reply + 28, net_my_ip, 4);     /* sender IP */
            mcpy(reply + 32, sender_mac, 6);    /* target MAC */
            mcpy(reply + 38, sender_ip, 4);     /* target IP */
            ip_send_raw(reply, 42);
        }
    }
}

/* ── ARP Resolve (blocking) ───────────────────────── */

int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out) {
    /* Cache hit? */
    if (arp_cache_lookup(ip, mac_out) == 0)
        return 0;

    /* Send ARP request */
    uint8_t pkt[42];
    mzero(pkt, 42);
    for (int i = 0; i < 6; i++) pkt[i] = 0xFF;
    mcpy(pkt + 6, net_my_mac, 6);
    put16(pkt + 12, 0x0806);
    put16(pkt + 14, 1); put16(pkt + 16, 0x0800);
    pkt[18] = 6; pkt[19] = 4; put16(pkt + 20, 1);
    mcpy(pkt + 22, net_my_mac, 6);
    mcpy(pkt + 28, net_my_ip, 4);
    mcpy(pkt + 38, ip, 4);
    ip_send_raw(pkt, 42);

    /* Wait for cache to be populated by arp_input (called from dispatcher) */
    uint64_t deadline = timer_ms() + NET_DHCP_RETRY_MS;
    while (timer_ms() < deadline) {
        if (arp_cache_lookup(ip, mac_out) == 0)
            return 0;
        net_idle();
    }
    return -1;
}
