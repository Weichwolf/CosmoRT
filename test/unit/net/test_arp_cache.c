/* test: ARP Cache — Hash-Table + Slab Pool
 * Replica of arp.c algorithm for unit testing.
 * Skal-D: hash-table (64 buckets) + pool (128 slots).
 */
#include "ktest.h"

/* ── Config ───────────────────────────────────────── */

#define TARP_POOL_SIZE 128
#define TARP_HASH_SIZE  64

/* ── Helpers ──────────────────────────────────────── */

static void tmcpy(void *d, const void *s, int n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
}

static int mac_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static inline uint32_t tip_to_u32(const uint8_t *ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8)  |  (uint32_t)ip[3];
}

/* ── ARP Entry ────────────────────────────────────── */

typedef struct tarp_entry {
    uint32_t           ip_key;
    uint8_t            mac[6];
    uint8_t            valid;
    uint8_t            _pad;
    struct tarp_entry *hash_next;
    int                pool_idx;   /* index in pool for free-list */
} tarp_entry_t;

/* ── Pool (simple free-list) ──────────────────────── */

static tarp_entry_t tarp_pool[TARP_POOL_SIZE];
static int          tarp_free_head;   /* index of first free, -1 = empty */
static int          tarp_free_next[TARP_POOL_SIZE]; /* next-free chain */

static void tarp_pool_init(void) {
    for (int i = 0; i < TARP_POOL_SIZE; i++) {
        tarp_pool[i].valid = 0;
        tarp_pool[i].hash_next = 0;
        tarp_pool[i].pool_idx = i;
        tarp_free_next[i] = i + 1;
    }
    tarp_free_next[TARP_POOL_SIZE - 1] = -1;
    tarp_free_head = 0;
}

static tarp_entry_t *tarp_alloc(void) {
    if (tarp_free_head < 0) return 0;
    int idx = tarp_free_head;
    tarp_free_head = tarp_free_next[idx];
    tarp_pool[idx].valid = 1;
    tarp_pool[idx].hash_next = 0;
    return &tarp_pool[idx];
}

static void tarp_free(tarp_entry_t *e) {
    int idx = e->pool_idx;
    e->valid = 0;
    e->hash_next = 0;
    tarp_free_next[idx] = tarp_free_head;
    tarp_free_head = idx;
}

/* ── Hash Table ───────────────────────────────────── */

static tarp_entry_t *tarp_hash[TARP_HASH_SIZE];
static int           tarp_evict_bucket;

static void tcache_reset(void) {
    tarp_pool_init();
    for (int i = 0; i < TARP_HASH_SIZE; i++)
        tarp_hash[i] = 0;
    tarp_evict_bucket = 0;
}

static int tcache_lookup(const uint8_t *ip, uint8_t *mac_out) {
    uint32_t key = tip_to_u32(ip);
    int idx = (int)(key & (TARP_HASH_SIZE - 1));
    for (tarp_entry_t *e = tarp_hash[idx]; e; e = e->hash_next) {
        if (e->ip_key == key) {
            tmcpy(mac_out, e->mac, 6);
            return 0;
        }
    }
    return -1;
}

static void evict_one(void) {
    for (int tries = 0; tries < TARP_HASH_SIZE; tries++) {
        int b = tarp_evict_bucket % TARP_HASH_SIZE;
        tarp_evict_bucket = b + 1;
        tarp_entry_t *e = tarp_hash[b];
        if (!e) continue;
        if (!e->hash_next) {
            tarp_hash[b] = 0;
            tarp_free(e);
            return;
        }
        tarp_entry_t *prev = e;
        while (prev->hash_next->hash_next)
            prev = prev->hash_next;
        tarp_free(prev->hash_next);
        prev->hash_next = 0;
        return;
    }
}

static void tcache_insert(const uint8_t *ip, const uint8_t *mac) {
    uint32_t key = tip_to_u32(ip);
    int idx = (int)(key & (TARP_HASH_SIZE - 1));

    /* Update existing? */
    for (tarp_entry_t *e = tarp_hash[idx]; e; e = e->hash_next) {
        if (e->ip_key == key) {
            tmcpy(e->mac, mac, 6);
            return;
        }
    }

    /* Allocate new */
    tarp_entry_t *n = tarp_alloc();
    if (!n) {
        evict_one();
        n = tarp_alloc();
        if (!n) return;
    }
    n->ip_key = key;
    tmcpy(n->mac, mac, 6);
    n->hash_next = tarp_hash[idx];
    tarp_hash[idx] = n;
}

static void tcache_evict(const uint8_t *ip) {
    uint32_t key = tip_to_u32(ip);
    int idx = (int)(key & (TARP_HASH_SIZE - 1));
    tarp_entry_t *prev = 0;
    for (tarp_entry_t *e = tarp_hash[idx]; e; prev = e, e = e->hash_next) {
        if (e->ip_key == key) {
            if (prev) prev->hash_next = e->hash_next;
            else      tarp_hash[idx] = e->hash_next;
            tarp_free(e);
            return;
        }
    }
}

/* ── Tests ─────────────────────────────────────────── */

static void test_arp_cache(void) {
    puts("\n[net/arp_cache]\n");
    tcache_reset();

    uint8_t mac[6];

    /* 1. Lookup on empty cache → miss */
    uint8_t ip1[4] = {10, 0, 2, 2};
    check("empty lookup miss", tcache_lookup(ip1, mac) == -1);

    /* 2. Insert + lookup → hit */
    uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    tcache_insert(ip1, mac1);
    check("insert+lookup hit", tcache_lookup(ip1, mac) == 0);
    check("mac matches", mac_eq(mac, mac1));

    /* 3. Update existing entry */
    uint8_t mac2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    tcache_insert(ip1, mac2);
    check("update lookup", tcache_lookup(ip1, mac) == 0);
    check("mac updated", mac_eq(mac, mac2));

    /* 4. Second IP */
    uint8_t ip2[4] = {10, 0, 2, 3};
    uint8_t mac3[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    tcache_insert(ip2, mac3);
    check("second ip lookup", tcache_lookup(ip2, mac) == 0);
    check("second mac matches", mac_eq(mac, mac3));
    check("first still present", tcache_lookup(ip1, mac) == 0);

    /* 5. Evict */
    tcache_evict(ip1);
    check("evicted miss", tcache_lookup(ip1, mac) == -1);
    check("other still present", tcache_lookup(ip2, mac) == 0);

    /* 6. Fill 64 entries (far beyond old 16-entry limit) */
    tcache_reset();
    for (int i = 0; i < 64; i++) {
        uint8_t ip[4] = {192, 168, (uint8_t)(i >> 8), (uint8_t)((i & 0xFF) + 1)};
        uint8_t m[6] = {0, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)(i + 1)};
        tcache_insert(ip, m);
    }
    int found = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t ip[4] = {192, 168, (uint8_t)(i >> 8), (uint8_t)((i & 0xFF) + 1)};
        if (tcache_lookup(ip, mac) == 0) found++;
    }
    check_val("64 entries all found", found, 64);

    /* 7. Hash collision: two IPs mapping to same bucket */
    tcache_reset();
    /* Bucket = ip & 63.  10.0.0.1 → bucket 1, 10.0.0.65 → bucket 1 */
    uint8_t cip1[4] = {10, 0, 0, 1};
    uint8_t cip2[4] = {10, 0, 0, 65};
    uint8_t cmac1[6] = {0xAA, 0, 0, 0, 0, 1};
    uint8_t cmac2[6] = {0xBB, 0, 0, 0, 0, 2};
    tcache_insert(cip1, cmac1);
    tcache_insert(cip2, cmac2);
    check("collision: first found", tcache_lookup(cip1, mac) == 0);
    check("collision: first mac ok", mac_eq(mac, cmac1));
    check("collision: second found", tcache_lookup(cip2, mac) == 0);
    check("collision: second mac ok", mac_eq(mac, cmac2));
    /* Evict first, second still there */
    tcache_evict(cip1);
    check("collision: evict first", tcache_lookup(cip1, mac) == -1);
    check("collision: second survives", tcache_lookup(cip2, mac) == 0);

    /* 8. Pool full: 128 entries, then insert 129th triggers eviction */
    tcache_reset();
    for (int i = 0; i < TARP_POOL_SIZE; i++) {
        uint8_t ip[4] = {10, (uint8_t)(i >> 8), (uint8_t)(i & 0xFF), 1};
        uint8_t m[6] = {0, 0, 0, 0, (uint8_t)(i >> 8), (uint8_t)(i & 0xFF)};
        tcache_insert(ip, m);
    }
    /* All 128 present */
    found = 0;
    for (int i = 0; i < TARP_POOL_SIZE; i++) {
        uint8_t ip[4] = {10, (uint8_t)(i >> 8), (uint8_t)(i & 0xFF), 1};
        if (tcache_lookup(ip, mac) == 0) found++;
    }
    check_val("pool full: 128 found", found, TARP_POOL_SIZE);

    /* Insert 129th → evicts one, new one findable */
    uint8_t overflow_ip[4] = {172, 16, 0, 1};
    uint8_t overflow_mac[6] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    tcache_insert(overflow_ip, overflow_mac);
    check("pool overflow: new findable", tcache_lookup(overflow_ip, mac) == 0);
    check("pool overflow: new mac ok", mac_eq(mac, overflow_mac));

    /* Total should still be 128 (one evicted) */
    found = 0;
    for (int i = 0; i < TARP_POOL_SIZE; i++) {
        uint8_t ip[4] = {10, (uint8_t)(i >> 8), (uint8_t)(i & 0xFF), 1};
        if (tcache_lookup(ip, mac) == 0) found++;
    }
    /* 127 of the original + 1 new = 128 total */
    check_val("pool overflow: 127 originals remain", found, TARP_POOL_SIZE - 1);

    /* 9. Evict nonexistent → no crash */
    uint8_t bogus[4] = {255, 255, 255, 255};
    tcache_evict(bogus);
    check("evict nonexistent ok", 1);
}

TEST("net/arp_cache", test_arp_cache);
