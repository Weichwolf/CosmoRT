/* CosmoRT ARP — Hash-Table Cache + Pending-Queue neighbour resolution.
 * Linux-Stil NUD-States. Requests senden, Reply flusht Queue, TTL-GC. */

#include "net/arp.h"
#include "net/ip.h"
#include "net/net.h"
#include "net/net_util.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/process.h"
#include "linux/errno.h"

/* ── Fast-Poll für synchrone Caller (DHCP Bring-Up) ───
 * 250ms reicht für LAN-RTT; 3s war Relikt aus dem blocking-API und
 * hielt den Kernel-Thread in net_idle fest.  Real-time-Requirement:
 * bounded execution. */
#define ARP_SYNC_TIMEOUT_MS 250

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
static int          arp_evict_bucket;

static inline int bucket_idx(uint32_t key) {
    return (int)(key & (ARP_HASH_SIZE - 1));
}

static arp_entry_t *arp_find_entry(uint32_t key) {
    for (arp_entry_t *e = arp_hash[bucket_idx(key)]; e; e = e->hash_next)
        if (e->ip_key == key) return e;
    return 0;
}

/* ── Cache Reset ──────────────────────────────────── */

void arp_cache_reset(void) {
    for (int i = 0; i < ARP_HASH_SIZE; i++)
        arp_hash[i] = 0;
    arp_evict_bucket = 0;
    arp_slab_inited = 0;
    arp_slab_ensure_init();
}

/* ── Evict one entry (tail of a non-empty bucket) ── */

static void evict_one(void) {
    for (int tries = 0; tries < ARP_HASH_SIZE; tries++) {
        int b = arp_evict_bucket % ARP_HASH_SIZE;
        arp_evict_bucket = b + 1;
        arp_entry_t *e = arp_hash[b];
        if (!e) continue;
        if (!e->hash_next) {
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

/* ── Lookup REACHABLE only ────────────────────────── */

int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out) {
    uint32_t key = ip_to_u32(ip);
    arp_entry_t *e = arp_find_entry(key);
    if (!e || e->state != NUD_REACHABLE) return -1;
    e->last_used_ms = timer_ms();
    mcpy(mac_out, e->mac, 6);
    return 0;
}

/* ── Allocate (or reuse) entry ────────────────────── */

static arp_entry_t *arp_alloc_entry(uint32_t key) {
    arp_slab_ensure_init();
    int idx = bucket_idx(key);

    for (arp_entry_t *e = arp_hash[idx]; e; e = e->hash_next)
        if (e->ip_key == key) return e;

    arp_entry_t *n = slab_alloc(&arp_slab);
    if (!n) {
        evict_one();
        n = slab_alloc(&arp_slab);
        if (!n) return 0;
    }
    n->ip_key = key;
    n->state = NUD_NONE;
    n->created_ms = timer_ms();
    n->last_used_ms = n->created_ms;
    n->pending_count = 0;
    n->hash_next = arp_hash[idx];
    arp_hash[idx] = n;
    return n;
}

/* ── Insert / Update (REACHABLE) ──────────────────── */

void arp_cache_insert(const uint8_t *ip, const uint8_t *mac) {
    uint32_t key = ip_to_u32(ip);
    arp_entry_t *e = arp_alloc_entry(key);
    if (!e) return;
    mcpy(e->mac, mac, 6);
    e->state = NUD_REACHABLE;
    e->created_ms = timer_ms();
    e->last_used_ms = e->created_ms;
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

/* ── Send ARP Request ─────────────────────────────── */

static void arp_send_request(const uint8_t *ip) {
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
}

/* ── Pending-Queue: enqueue frame ─────────────────── */

static int pending_enqueue(arp_entry_t *e, const uint8_t *frame, int len) {
    if (e->pending_count >= ARP_PENDING_PER_ENTRY) return -1;
    if (len <= 0 || len > ARP_PENDING_PKT_SIZE) return -1;
    arp_pending_t *slot = &e->pending[e->pending_count++];
    slot->len = (uint16_t)len;
    mcpy(slot->data, frame, len);
    return 0;
}

/* ── Pending-Queue: flush on reply ────────────────── */

static void pending_flush(arp_entry_t *e) {
    for (int i = 0; i < e->pending_count; i++) {
        arp_pending_t *p = &e->pending[i];
        /* Rewrite destination MAC (bytes 0..5) with freshly learned MAC. */
        mcpy(p->data, e->mac, 6);
        net_send_raw(p->data, p->len);
    }
    e->pending_count = 0;
}

/* ── Queue-or-send API ────────────────────────────── */

int net_arp_queue_frame(const uint8_t *ip, const uint8_t *frame, int len) {
    uint32_t key = ip_to_u32(ip);
    arp_entry_t *e = arp_find_entry(key);

    if (e && e->state == NUD_REACHABLE) {
        /* Overwrite destination MAC and send directly. */
        uint8_t copy[ARP_PENDING_PKT_SIZE];
        if (len <= 0 || len > (int)sizeof(copy)) return -EHOSTUNREACH;
        mcpy(copy, frame, len);
        mcpy(copy, e->mac, 6);
        net_send_raw(copy, len);
        e->last_used_ms = timer_ms();
        return 0;
    }

    if (!e) {
        e = arp_alloc_entry(key);
        if (!e) return -EHOSTUNREACH;
        e->state = NUD_INCOMPLETE;
        arp_send_request(ip);
    } else if (e->state == NUD_FAILED) {
        return -EHOSTUNREACH;
    }

    if (pending_enqueue(e, frame, len) < 0) return -EHOSTUNREACH;
    return 0;
}

/* ── ARP Input ────────────────────────────────────── */

void arp_input(const uint8_t *pkt, int len) {
    if (len < 42) return;
    uint16_t op = get16(pkt + 20);

    const uint8_t *sender_mac = pkt + 22;
    const uint8_t *sender_ip  = pkt + 28;

    /* Filter bogus MACs: all-FF or all-00 */
    if ((sender_mac[0] == 0xFF && sender_mac[1] == 0xFF && sender_mac[2] == 0xFF &&
         sender_mac[3] == 0xFF && sender_mac[4] == 0xFF && sender_mac[5] == 0xFF) ||
        (sender_mac[0] == 0 && sender_mac[1] == 0 && sender_mac[2] == 0 &&
         sender_mac[3] == 0 && sender_mac[4] == 0 && sender_mac[5] == 0))
        return;

    if (op == 2) {
        /* ARP Reply — update cache, flush pending. */
        uint32_t key = ip_to_u32(sender_ip);
        arp_entry_t *e = arp_alloc_entry(key);
        if (!e) return;
        mcpy(e->mac, sender_mac, 6);
        int was_incomplete = (e->state == NUD_INCOMPLETE);
        e->state = NUD_REACHABLE;
        e->created_ms = timer_ms();
        e->last_used_ms = e->created_ms;
        if (was_incomplete)
            pending_flush(e);
    } else if (op == 1) {
        /* ARP Request — if target IP is ours, send reply. */
        const uint8_t *target_ip = pkt + 38;
        if (net_my_ip[0] && target_ip[0] == net_my_ip[0] &&
            target_ip[1] == net_my_ip[1] && target_ip[2] == net_my_ip[2] &&
            target_ip[3] == net_my_ip[3]) {
            arp_cache_insert(sender_ip, sender_mac);

            uint8_t reply[42];
            mzero(reply, 42);
            mcpy(reply, sender_mac, 6);
            mcpy(reply + 6, net_my_mac, 6);
            put16(reply + 12, 0x0806);
            put16(reply + 14, 1);
            put16(reply + 16, 0x0800);
            reply[18] = 6; reply[19] = 4;
            put16(reply + 20, 2);
            mcpy(reply + 22, net_my_mac, 6);
            mcpy(reply + 28, net_my_ip, 4);
            mcpy(reply + 32, sender_mac, 6);
            mcpy(reply + 38, sender_ip, 4);
            ip_send_raw(reply, 42);
        }
    }
}

/* ── Periodic GC ──────────────────────────────────── */

void arp_cache_gc(void) {
    uint64_t now = timer_ms();
    for (int b = 0; b < ARP_HASH_SIZE; b++) {
        arp_entry_t **pp = &arp_hash[b];
        while (*pp) {
            arp_entry_t *e = *pp;
            int expire = 0;
            if (e->state == NUD_INCOMPLETE &&
                now - e->created_ms > ARP_INCOMPLETE_TTL_MS) {
                e->state = NUD_FAILED;
                e->pending_count = 0;
                expire = 1;
            } else if (e->state == NUD_REACHABLE &&
                       now - e->last_used_ms > ARP_REACHABLE_TTL_MS) {
                expire = 1;
            } else if (e->state == NUD_FAILED) {
                expire = 1;
            }
            if (expire) {
                *pp = e->hash_next;
                slab_free(&arp_slab, e);
            } else {
                pp = &e->hash_next;
            }
        }
    }
}

/* ── Synchronous Resolve (bestehende Callers) ─────── */

int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out) {
    if (arp_cache_lookup(ip, mac_out) == 0)
        return 0;

    /* Trigger INCOMPLETE entry + request.  Same state machine als
     * net_arp_queue_frame, aber ohne Frame (Caller hat keinen). */
    uint32_t key = ip_to_u32(ip);
    arp_entry_t *e = arp_alloc_entry(key);
    if (!e) return -EHOSTUNREACH;
    if (e->state != NUD_INCOMPLETE) {
        e->state = NUD_INCOMPLETE;
        e->created_ms = timer_ms();
        e->pending_count = 0;
        arp_send_request(ip);
    }

    /* Bounded poll: 250ms max, signal-interruptible.  Kein net_idle
     * über mehrere Sekunden — das verletzt RT-Bounded-Execution. */
    uint64_t deadline = timer_ms() + ARP_SYNC_TIMEOUT_MS;
    while (timer_ms() < deadline) {
        if (arp_cache_lookup(ip, mac_out) == 0)
            return 0;
        process_t *p = proc_current();
        thread_t  *t = thread_current();
        if (p && t && ((p->sig_pending | t->sig_thread_pending) & ~t->sig_blocked))
            return -EHOSTUNREACH;
        net_idle();
    }
    return -EHOSTUNREACH;
}
