/* CosmoRT NDP — RFC 4861 Neighbor Discovery + RFC 4862 SLAAC link-local.
 *
 * Per-NS hash table keyed on the lower 32 bits of the in6_addr (good
 * spread for /64 LANs). Entries are slab-allocated; full ageing (Linux
 * gc_thresh) is left for a later pass since loopback never expires
 * REACHABLE state.
 */

#include "net/ndp.h"
#include "net/in6.h"
#include "net/ipv6.h"
#include "net/icmpv6.h"
#include "net/net.h"
#include "net/net_ns.h"
#include "net/netif.h"
#include "net/net_util.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "core/timer.h"
#include "linux/errno.h"

#define NDP_HASH_SIZE  64

static ndp_entry_t *ndp_hash[NDP_HASH_SIZE];
static spinlock_t   ndp_lock = SPINLOCK_INIT;
static slab_t       ndp_slab;
static int          ndp_inited;

static void ndp_slab_ensure(void) {
    if (__builtin_expect(ndp_inited, 1)) return;
    slab_init_dynamic(&ndp_slab, (int)sizeof(ndp_entry_t), 4);
    ndp_inited = 1;
}

void ndp_init(void) { ndp_slab_ensure(); }

static uint32_t addr_hash(const struct in6_addr *a) {
    uint32_t v = 0;
    for (int i = 12; i < 16; i++) v = (v << 8) | a->s6_addr[i];
    return v & (NDP_HASH_SIZE - 1);
}

static ndp_entry_t *find_entry_locked(uint32_t ns_id, const struct in6_addr *addr) {
    uint32_t idx = addr_hash(addr);
    for (ndp_entry_t *e = ndp_hash[idx]; e; e = e->next)
        if (e->ns_id == ns_id && in6_eq(&e->addr, addr)) return e;
    return 0;
}

int ndp_lookup(uint32_t ns_id, const struct in6_addr *addr, uint8_t *mac_out) {
    uint64_t flags;
    spin_lock_irq(&ndp_lock, &flags);
    ndp_slab_ensure();
    ndp_entry_t *e = find_entry_locked(ns_id, addr);
    int rc = -1;
    if (e && e->state == NUD6_REACHABLE) {
        for (int i = 0; i < 6; i++) mac_out[i] = e->mac[i];
        e->last_used_ms = timer_ms();
        rc = 0;
    }
    spin_unlock_irq(&ndp_lock, flags);
    return rc;
}

void ndp_remember(uint32_t ns_id, const struct in6_addr *addr, const uint8_t *mac) {
    uint64_t flags;
    spin_lock_irq(&ndp_lock, &flags);
    ndp_slab_ensure();
    ndp_entry_t *e = find_entry_locked(ns_id, addr);
    if (!e) {
        e = (ndp_entry_t *)slab_alloc(&ndp_slab);
        if (!e) { spin_unlock_irq(&ndp_lock, flags); return; }
        in6_copy(&e->addr, addr);
        e->ns_id = ns_id;
        uint32_t idx = addr_hash(addr);
        e->next = ndp_hash[idx];
        ndp_hash[idx] = e;
    }
    for (int i = 0; i < 6; i++) e->mac[i] = mac[i];
    e->state = NUD6_REACHABLE;
    e->last_used_ms = timer_ms();
    spin_unlock_irq(&ndp_lock, flags);
}

/* ── Send Neighbor Solicitation ─────────────────────
 *
 * RFC 4861 §4.3:
 *   Reserved(4) + Target Address(16) + Options...
 *
 * Linux always includes the Source Link-Layer Address option (Type=1)
 * unless the IP source is :: (DAD case). */
static void ndp_send_ns(uint32_t ns_id,
                        const struct in6_addr *src,
                        const struct in6_addr *target,
                        int dad) {
    uint8_t pkt[1536];
    mzero(pkt, sizeof(pkt));

    /* Solicited-node multicast for target: ff02::1:ff<lower 24 bits> */
    struct in6_addr mc;
    for (int i = 0; i < 16; i++) mc.s6_addr[i] = 0;
    mc.s6_addr[0]  = 0xFF; mc.s6_addr[1] = 0x02;
    mc.s6_addr[11] = 0x01; mc.s6_addr[12] = 0xFF;
    mc.s6_addr[13] = target->s6_addr[13];
    mc.s6_addr[14] = target->s6_addr[14];
    mc.s6_addr[15] = target->s6_addr[15];

    /* MAC for solicited-node multicast: 33:33:ff:<low24> */
    uint8_t mc_mac[6] = {0x33, 0x33, 0xFF,
                         target->s6_addr[13],
                         target->s6_addr[14],
                         target->s6_addr[15]};

    int icmp_len = 4 + 16 + (dad ? 0 : 8); /* + SLLA option */
    ipv6_build_header(pkt, mc_mac, src, &mc, IPPROTO_ICMPV6,
                      (uint16_t)icmp_len, 255);

    uint8_t *ic = pkt + 54;
    ic[0] = ICMPV6_NB_SOLICIT;
    ic[1] = 0;
    ic[2] = 0; ic[3] = 0;        /* cksum */
    ic[4] = ic[5] = ic[6] = ic[7] = 0; /* reserved */
    for (int i = 0; i < 16; i++) ic[8 + i] = target->s6_addr[i];
    if (!dad) {
        ic[24] = 1;              /* Source Link-Layer Address option */
        ic[25] = 1;              /* length in 8-byte units */
        for (int i = 0; i < 6; i++) ic[26 + i] = net_my_mac[i];
    }
    uint16_t c = icmpv6_cksum(src, &mc, ic, icmp_len);
    ic[2] = (uint8_t)(c >> 8);
    ic[3] = (uint8_t)c;

    (void)ns_id;
    ipv6_send_frame(pkt, (uint16_t)(54 + icmp_len));
}

void ndp_send_dad_ns(uint32_t ns_id, const struct in6_addr *target) {
    /* DAD source = :: */
    ndp_send_ns(ns_id, &in6addr_any, target, 1);
}

/* ── Send Neighbor Advertisement ───────────────────
 *
 * RFC 4861 §4.4: Flags(R/S/O) + Reserved(29 bits) + Target + TLLA option.
 * Solicited (S=1) means in response to an NS targeting us. */
static void ndp_send_na(uint32_t ns_id,
                        const struct in6_addr *src_target,
                        const struct in6_addr *dst,
                        const uint8_t *dst_mac,
                        int solicited) {
    uint8_t pkt[1536];
    mzero(pkt, sizeof(pkt));

    int icmp_len = 4 + 16 + 8; /* hdr + target + TLLA opt */
    ipv6_build_header(pkt, dst_mac, src_target, dst, IPPROTO_ICMPV6,
                      (uint16_t)icmp_len, 255);

    uint8_t *ic = pkt + 54;
    ic[0] = ICMPV6_NB_ADVERT;
    ic[1] = 0;
    ic[2] = 0; ic[3] = 0;
    /* Flags: R=0 (we're a host), S=solicited, O=override on first NA. */
    uint8_t flags = (uint8_t)((solicited ? 0x40 : 0) | 0x20 /* O */);
    ic[4] = flags;
    ic[5] = ic[6] = ic[7] = 0;
    for (int i = 0; i < 16; i++) ic[8 + i] = src_target->s6_addr[i];
    /* Target Link-Layer Address option */
    ic[24] = 2; ic[25] = 1;
    for (int i = 0; i < 6; i++) ic[26 + i] = net_my_mac[i];

    uint16_t c = icmpv6_cksum(src_target, dst, ic, icmp_len);
    ic[2] = (uint8_t)(c >> 8);
    ic[3] = (uint8_t)c;

    (void)ns_id;
    ipv6_send_frame(pkt, (uint16_t)(54 + icmp_len));
}

/* ── Input handlers ─────────────────────────────── */

void ndp_ns_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    if (p->l4_len < 4 + 16) return;
    const uint8_t *ic = p->frame + p->l4_off;
    /* Hop-Limit must be 255 for valid NDP (RFC 4861 §6.1.1). */
    if (p->hop_limit != 255) return;

    struct in6_addr target;
    for (int i = 0; i < 16; i++) target.s6_addr[i] = ic[8 + i];

    /* If target is one of our addresses, reply with NA. */
    if (!ipv6_addr_local(ns_id, &target)) return;

    /* Source MAC from Ethernet frame (offset 6). */
    uint8_t src_mac[6];
    for (int i = 0; i < 6; i++) src_mac[i] = p->frame[6 + i];

    /* Remember requester's address↔MAC for return path. */
    if (!in6_is_any(&p->src)) ndp_remember(ns_id, &p->src, src_mac);

    /* Solicited reply: send NA back to the originator. */
    const struct in6_addr *reply_dst = in6_is_any(&p->src)
        ? &in6addr_loopback /* DAD probe: drop, would conflict on real LAN */
        : &p->src;
    if (in6_is_any(&p->src)) return;
    ndp_send_na(ns_id, &target, reply_dst, src_mac, 1);
}

void ndp_na_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    if (p->l4_len < 4 + 16) return;
    const uint8_t *ic = p->frame + p->l4_off;
    if (p->hop_limit != 255) return;

    struct in6_addr target;
    for (int i = 0; i < 16; i++) target.s6_addr[i] = ic[8 + i];

    /* Look for TLLA option. */
    int opt = 24;
    while (opt + 2 <= p->l4_len) {
        uint8_t kind = ic[opt];
        uint8_t olen = ic[opt + 1];
        if (olen == 0) break;
        int blen = olen * 8;
        if (opt + blen > p->l4_len) break;
        if (kind == 2 && blen >= 8) {
            /* Target Link-Layer Address */
            ndp_remember(ns_id, &target, ic + opt + 2);
            return;
        }
        opt += blen;
    }
    /* No TLLA: still mark REACHABLE with the Ethernet src MAC. */
    uint8_t src_mac[6];
    for (int i = 0; i < 6; i++) src_mac[i] = p->frame[6 + i];
    ndp_remember(ns_id, &target, src_mac);
}

void ndp_rs_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    /* Hosts ignore RS. */
    (void)ns_id; (void)p;
}

void ndp_ra_input(uint32_t ns_id, const ipv6_pkt_t *p) {
    /* RFC 4862 §5.5.3: walk Prefix Information options, attach SLAAC
     * addresses for those with A=1 + on-link L=1 prefix /64. */
    if (p->l4_len < 16) return;
    const uint8_t *ic = p->frame + p->l4_off;
    if (p->hop_limit != 255) return;

    int opt = 16;
    while (opt + 2 <= p->l4_len) {
        uint8_t kind = ic[opt];
        uint8_t olen = ic[opt + 1];
        if (olen == 0) break;
        int blen = olen * 8;
        if (opt + blen > p->l4_len) break;
        if (kind == 3 && blen >= 32) {
            /* Prefix Information Option (RFC 4861 §4.6.2). */
            uint8_t prefix_len = ic[opt + 2];
            uint8_t flags      = ic[opt + 3];
            if ((flags & 0x40) /* A flag */ && prefix_len == 64) {
                struct in6_addr glob;
                for (int i = 0; i < 16; i++) glob.s6_addr[i] = ic[opt + 16 + i];
                /* Append our EUI-64-derived host id to the prefix. */
                struct in6_addr ll;
                ndp_make_linklocal(net_my_mac, &ll);
                for (int i = 8; i < 16; i++) glob.s6_addr[i] = ll.s6_addr[i];
                struct netif *def = netif_default_ns(0);
                if (def) {
                    extern int ipv6_addr_add(struct net_ns *, struct netif *,
                                             const struct in6_addr *, uint8_t);
                    ipv6_addr_add(&init_net_ns, def, &glob, 64);
                }
            }
        }
        opt += blen;
    }
    (void)ns_id;
}

/* ── Resolve (used by tcp6/udp6 send path) ───────── */

int ndp_resolve(uint32_t ns_id, const struct in6_addr *addr, uint8_t *mac_out) {
    /* Loopback short-circuit. */
    if (in6_is_loopback(addr)) {
        for (int i = 0; i < 6; i++) mac_out[i] = 0;
        return 0;
    }
    if (ndp_lookup(ns_id, addr, mac_out) == 0) return 0;
    /* Fire NS, return -EAGAIN — caller retries via syscall restart. */
    struct in6_addr src;
    ipv6_select_src(ns_id, addr, &src);
    ndp_send_ns(ns_id, &src, addr, 0);
    return -EAGAIN;
}

/* ── Link-local address generation (RFC 4862 §5.3 / RFC 4291 App A) ──
 *
 * EUI-64 from MAC: invert the universal/local bit (bit 1 of the first
 * byte), insert FF:FE in the middle. Result: fe80::<EUI64>. */
void ndp_make_linklocal(const uint8_t mac[6], struct in6_addr *out) {
    for (int i = 0; i < 16; i++) out->s6_addr[i] = 0;
    out->s6_addr[0] = 0xFE;
    out->s6_addr[1] = 0x80;
    out->s6_addr[8]  = mac[0] ^ 0x02; /* flip U/L bit */
    out->s6_addr[9]  = mac[1];
    out->s6_addr[10] = mac[2];
    out->s6_addr[11] = 0xFF;
    out->s6_addr[12] = 0xFE;
    out->s6_addr[13] = mac[3];
    out->s6_addr[14] = mac[4];
    out->s6_addr[15] = mac[5];
}

/* ── Self-tests (called by ktest) ──────────────── */

int ipv6_self_test_linklocal(void) {
    /* fe80::<eui64> for MAC 02:11:22:33:44:55 → fe80::11:22ff:fe33:4455 */
    uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    struct in6_addr ll;
    ndp_make_linklocal(mac, &ll);
    if (ll.s6_addr[0]  != 0xFE || ll.s6_addr[1]  != 0x80) return 1;
    if (ll.s6_addr[8]  != 0x00 || ll.s6_addr[9]  != 0x11) return 2;
    if (ll.s6_addr[10] != 0x22 || ll.s6_addr[11] != 0xFF) return 3;
    if (ll.s6_addr[12] != 0xFE || ll.s6_addr[13] != 0x33) return 4;
    if (ll.s6_addr[14] != 0x44 || ll.s6_addr[15] != 0x55) return 5;
    return 0;
}

int ipv6_self_test_neigh(void) {
    /* Lookup of a never-seen address must miss. */
    struct in6_addr a;
    for (int i = 0; i < 16; i++) a.s6_addr[i] = 0;
    a.s6_addr[0] = 0x20; a.s6_addr[15] = 0xab;
    uint8_t mac[6];
    if (ndp_lookup(init_net_ns.ns_id, &a, mac) == 0) return 1;
    /* Insert + lookup hits. */
    uint8_t want[6] = {1,2,3,4,5,6};
    ndp_remember(init_net_ns.ns_id, &a, want);
    if (ndp_lookup(init_net_ns.ns_id, &a, mac) != 0) return 2;
    for (int i = 0; i < 6; i++) if (mac[i] != want[i]) return 3;
    return 0;
}
