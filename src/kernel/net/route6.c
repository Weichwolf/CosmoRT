/* CosmoRT IPv6 Route Table — per-NS prefix list.
 *
 * Each NS owns a singly-linked list of route6_entry. Implicit routes:
 *   - ::1/128  → loopback (added at route6_init for init_net_ns; per-NS
 *                lo gets it when net_ns_alloc calls route6_attach_lo).
 *   - <my-link-local>/64 → default NIC (added by ipv6_iface_bringup).
 *
 * Lookup walks the list for longest-prefix-match (LPM); stable order
 * means specific routes (like ::1/128) added later beat default routes.
 */

#include "net/route6.h"
#include "net/in6.h"
#include "net/net_ns.h"
#include "net/netif.h"
#include "mm/slab.h"
#include "spinlock.h"

/* Per-NS list head — kept in a side-table because struct net_ns is in
 * Phase-15 design and we don't want to touch its layout for every new
 * subsystem. Slab-backed map (ns_id → head). */

typedef struct ns_route_head {
    uint32_t       ns_id;
    route6_entry_t *head;
    struct ns_route_head *next;
} ns_route_head_t;

static slab_t route6_slab;
static slab_t route6_head_slab;
static int    route6_inited;
static ns_route_head_t *heads;
static spinlock_t route6_lock = SPINLOCK_INIT;

static void route6_slabs_ensure(void) {
    if (__builtin_expect(route6_inited, 1)) return;
    slab_init_dynamic(&route6_slab,      (int)sizeof(route6_entry_t), 4);
    slab_init_dynamic(&route6_head_slab, (int)sizeof(ns_route_head_t), 4);
    route6_inited = 1;
}

static ns_route_head_t *find_head(uint32_t ns_id, int alloc) {
    for (ns_route_head_t *h = heads; h; h = h->next)
        if (h->ns_id == ns_id) return h;
    if (!alloc) return 0;
    ns_route_head_t *h = (ns_route_head_t *)slab_alloc(&route6_head_slab);
    if (!h) return 0;
    h->ns_id = ns_id;
    h->head  = 0;
    h->next  = heads;
    heads = h;
    return h;
}

/* Bit-level prefix match: count of common prefix bits. */
static int common_bits(const struct in6_addr *a, const struct in6_addr *b) {
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t x = a->s6_addr[i] ^ b->s6_addr[i];
        if (x == 0) { n += 8; continue; }
        for (int b2 = 7; b2 >= 0; b2--) {
            if (x & (1 << b2)) return n;
            n++;
        }
        return n;
    }
    return n;
}

int route6_add(struct net_ns *ns, const struct in6_addr *prefix,
               uint8_t prefix_len, const struct in6_addr *gateway,
               struct netif *oif) {
    if (!ns) ns = &init_net_ns;
    uint64_t flags;
    spin_lock_irq(&route6_lock, &flags);
    route6_slabs_ensure();
    ns_route_head_t *h = find_head(ns->ns_id, 1);
    if (!h) { spin_unlock_irq(&route6_lock, flags); return -1; }
    /* Dedup */
    for (route6_entry_t *e = h->head; e; e = e->next)
        if (e->prefix_len == prefix_len && in6_eq(&e->prefix, prefix)) {
            spin_unlock_irq(&route6_lock, flags); return 0;
        }
    route6_entry_t *e = (route6_entry_t *)slab_alloc(&route6_slab);
    if (!e) { spin_unlock_irq(&route6_lock, flags); return -1; }
    in6_copy(&e->prefix, prefix);
    e->prefix_len = prefix_len;
    e->metric     = 0;
    if (gateway) in6_copy(&e->gateway, gateway);
    else         in6_copy(&e->gateway, &in6addr_any);
    e->oif  = oif;
    /* Insert sorted by descending prefix_len so first match wins LPM. */
    route6_entry_t **pp = &h->head;
    while (*pp && (*pp)->prefix_len >= prefix_len) pp = &(*pp)->next;
    e->next = *pp;
    *pp = e;
    spin_unlock_irq(&route6_lock, flags);
    return 0;
}

route6_entry_t *route6_lookup(struct net_ns *ns, const struct in6_addr *dst) {
    if (!ns) ns = &init_net_ns;
    uint64_t flags;
    spin_lock_irq(&route6_lock, &flags);
    ns_route_head_t *h = find_head(ns->ns_id, 0);
    route6_entry_t *best = 0;
    if (h) {
        for (route6_entry_t *e = h->head; e; e = e->next) {
            int bits = common_bits(&e->prefix, dst);
            if (bits >= e->prefix_len) { best = e; break; /* sorted, first match wins */ }
        }
    }
    spin_unlock_irq(&route6_lock, flags);
    return best;
}

/* Called by net_ns_alloc to attach the implicit ::1 route to a fresh NS. */
void route6_attach_lo(struct net_ns *ns) {
    if (!ns) ns = &init_net_ns;
    struct netif *lo = netif_loopback_ns(ns);
    if (!lo) return;
    struct in6_addr p = in6addr_loopback;
    route6_add(ns, &p, 128, &in6addr_any, lo);
}

void route6_init(void) {
    route6_slabs_ensure();
    /* ::1/128 → init NS loopback. */
    struct netif *lo = netif_loopback_ns(&init_net_ns);
    if (lo) {
        struct in6_addr p = in6addr_loopback;
        route6_add(&init_net_ns, &p, 128, &in6addr_any, lo);
    }
}

/* Self-test for ktest. */
int ipv6_self_test_route(void) {
    /* Ensure ::1 → init lo regardless of test ordering. */
    route6_init();
    route6_entry_t *r = route6_lookup(&init_net_ns, &in6addr_loopback);
    if (!r)                     return 1;
    if (r->prefix_len != 128)   return 2;
    if (!r->oif)                return 3;
    if (!(r->oif->flags & NETIF_F_LOOPBACK)) return 4;
    return 0;
}
