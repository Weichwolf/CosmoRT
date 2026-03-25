/* test: ARP Cache — Insert, Lookup, Evict, Round-Robin Eviction
 * Tests the cache algorithm in isolation (same logic as arp.c).
 */
#include "ktest.h"

/* ── Minimal ARP cache replica for unit testing ────── */

#define TEST_ARP_CACHE 16

static void tmcpy(void *d, const void *s, int n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
}

static int ip4_eq(const uint8_t *a, const uint8_t *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

static int mac_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t valid;
} test_arp_entry_t;

static test_arp_entry_t tcache[TEST_ARP_CACHE];
static int tcache_next;

static int tcache_lookup(const uint8_t *ip, uint8_t *mac_out) {
    for (int i = 0; i < TEST_ARP_CACHE; i++) {
        if (tcache[i].valid && ip4_eq(tcache[i].ip, ip)) {
            tmcpy(mac_out, tcache[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

static void tcache_insert(const uint8_t *ip, const uint8_t *mac) {
    for (int i = 0; i < TEST_ARP_CACHE; i++) {
        if (tcache[i].valid && ip4_eq(tcache[i].ip, ip)) {
            tmcpy(tcache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < TEST_ARP_CACHE; i++) {
        if (!tcache[i].valid) {
            tmcpy(tcache[i].ip, ip, 4);
            tmcpy(tcache[i].mac, mac, 6);
            tcache[i].valid = 1;
            return;
        }
    }
    int idx = tcache_next % TEST_ARP_CACHE;
    tmcpy(tcache[idx].ip, ip, 4);
    tmcpy(tcache[idx].mac, mac, 6);
    tcache[idx].valid = 1;
    tcache_next = idx + 1;
}

static void tcache_evict(const uint8_t *ip) {
    for (int i = 0; i < TEST_ARP_CACHE; i++) {
        if (tcache[i].valid && ip4_eq(tcache[i].ip, ip)) {
            tcache[i].valid = 0;
            return;
        }
    }
}

static void tcache_reset(void) {
    for (int i = 0; i < TEST_ARP_CACHE; i++) tcache[i].valid = 0;
    tcache_next = 0;
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

    /* 6. Fill cache to capacity */
    tcache_reset();
    for (int i = 0; i < TEST_ARP_CACHE; i++) {
        uint8_t ip[4] = {192, 168, 1, (uint8_t)(i + 1)};
        uint8_t m[6] = {0, 0, 0, 0, 0, (uint8_t)(i + 1)};
        tcache_insert(ip, m);
    }
    /* All 16 should be findable */
    int found = 0;
    for (int i = 0; i < TEST_ARP_CACHE; i++) {
        uint8_t ip[4] = {192, 168, 1, (uint8_t)(i + 1)};
        if (tcache_lookup(ip, mac) == 0 && mac[5] == (uint8_t)(i + 1))
            found++;
    }
    check_val("full cache all found", found, TEST_ARP_CACHE);

    /* 7. Overflow: 17th entry evicts one */
    uint8_t ip17[4] = {192, 168, 1, 17};
    uint8_t mac17[6] = {0, 0, 0, 0, 0, 17};
    tcache_insert(ip17, mac17);
    check("overflow: new findable", tcache_lookup(ip17, mac) == 0);
    check("overflow: new mac ok", mac[5] == 17);

    /* Count remaining: should be 16 (one was evicted) */
    found = 0;
    for (int i = 0; i < 17; i++) {
        uint8_t ip[4] = {192, 168, 1, (uint8_t)(i + 1)};
        if (tcache_lookup(ip, mac) == 0) found++;
    }
    check_val("overflow: total entries", found, TEST_ARP_CACHE);

    /* 8. Evict nonexistent → no crash */
    uint8_t bogus[4] = {255, 255, 255, 255};
    tcache_evict(bogus);
    check("evict nonexistent ok", 1);
}

TEST("net/arp_cache", test_arp_cache);
