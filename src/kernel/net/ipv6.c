/* CosmoRT IPv6 — header parser, send path, address-list per netif.
 *
 * Wire packets enter via dispatch.c on EtherType 0x86DD; ipv6_parse walks
 * the extension chain (Hop-by-Hop, Routing, Fragment, DstOpts) until it
 * lands on TCP/UDP/ICMPv6 and then dispatches.
 *
 * Per-netif address list keyed by struct in6_addr; bind() consults
 * ipv6_addr_local() to decide if a sin6_addr is bindable. ::1 is always
 * valid because the loopback netif always carries it. */

#include "net/ipv6.h"
#include "net/in6.h"
#include "net/net.h"
#include "net/net_ns.h"
#include "net/netif.h"
#include "net/net_util.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "hw/serial.h"

/* Forward demuxers — declared weak so commits land incrementally;
 * the strong definitions arrive with their respective subsystems. */
__attribute__((weak)) void icmpv6_input(uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; }
__attribute__((weak)) void tcp6_input  (uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; }
__attribute__((weak)) int  udp6_input  (uint32_t ns_id, const ipv6_pkt_t *p) { (void)ns_id; (void)p; return 0; }

/* ── netif ↔ in6_addr list ─────────────────────────
 *
 * struct netif has no per-protocol slot in CosmoRT, so we keep a tiny
 * side-table mapping (ns_id, netif_ptr) → in6_addr list. Slab-allocated;
 * walked rarely (bind, addr selection). */

typedef struct ipv6_netif_entry {
    struct net_ns *ns;
    struct netif  *nif;
    ipv6_addr_t   *addrs;
    struct ipv6_netif_entry *next;
} ipv6_netif_entry_t;

static slab_t ipv6_addr_slab;
static slab_t ipv6_netif_slab;
static int    ipv6_slabs_inited;
static ipv6_netif_entry_t *ipv6_netif_head;
static spinlock_t ipv6_lock = SPINLOCK_INIT;

static void ipv6_slabs_ensure(void) {
    if (__builtin_expect(ipv6_slabs_inited, 1)) return;
    slab_init_dynamic(&ipv6_addr_slab,  (int)sizeof(ipv6_addr_t),       4);
    slab_init_dynamic(&ipv6_netif_slab, (int)sizeof(ipv6_netif_entry_t), 4);
    ipv6_slabs_inited = 1;
}

static ipv6_netif_entry_t *find_or_alloc_entry(struct net_ns *ns, struct netif *nif) {
    for (ipv6_netif_entry_t *e = ipv6_netif_head; e; e = e->next)
        if (e->ns == ns && e->nif == nif) return e;
    ipv6_netif_entry_t *e = (ipv6_netif_entry_t *)slab_alloc(&ipv6_netif_slab);
    if (!e) return 0;
    e->ns = ns; e->nif = nif; e->addrs = 0;
    e->next = ipv6_netif_head;
    ipv6_netif_head = e;
    return e;
}

static int ipv6_addr_attach(struct net_ns *ns, struct netif *nif,
                            const struct in6_addr *a, uint8_t prefix,
                            int is_loopback) {
    uint64_t flags;
    spin_lock_irq(&ipv6_lock, &flags);
    ipv6_slabs_ensure();
    ipv6_netif_entry_t *e = find_or_alloc_entry(ns, nif);
    if (!e) { spin_unlock_irq(&ipv6_lock, flags); return -1; }
    /* dedup */
    for (ipv6_addr_t *x = e->addrs; x; x = x->next)
        if (in6_eq(&x->addr, a)) { spin_unlock_irq(&ipv6_lock, flags); return 0; }
    ipv6_addr_t *na = (ipv6_addr_t *)slab_alloc(&ipv6_addr_slab);
    if (!na) { spin_unlock_irq(&ipv6_lock, flags); return -1; }
    in6_copy(&na->addr, a);
    na->prefix_len  = prefix;
    na->dad_state   = is_loopback ? IPV6_DAD_PREFERRED : IPV6_DAD_TENTATIVE;
    na->is_loopback = (uint8_t)is_loopback;
    na->next = e->addrs;
    e->addrs = na;
    spin_unlock_irq(&ipv6_lock, flags);
    return 0;
}

static int ipv6_addr_match_in_ns(uint32_t ns_id, const struct in6_addr *a) {
    int hit = 0;
    uint64_t flags;
    spin_lock_irq(&ipv6_lock, &flags);
    for (ipv6_netif_entry_t *e = ipv6_netif_head; e && !hit; e = e->next) {
        if (e->ns->ns_id != ns_id) continue;
        for (ipv6_addr_t *x = e->addrs; x; x = x->next)
            if (in6_eq(&x->addr, a)) { hit = 1; break; }
    }
    spin_unlock_irq(&ipv6_lock, flags);
    return hit;
}

int ipv6_addr_local(uint32_t ns_id, const struct in6_addr *a) {
    if (in6_is_any(a))      return 1;     /* :: bind always allowed */
    if (in6_is_loopback(a)) return 1;     /* ::1 always present */
    return ipv6_addr_match_in_ns(ns_id, a);
}

/* ── Header parser ──────────────────────────────── */

int ipv6_parse(const uint8_t *frame, int len, int eth_off, ipv6_pkt_t *out) {
    if (len < eth_off + IPV6_HDR_LEN) return -1;
    const uint8_t *h = frame + eth_off;

    uint8_t ver = h[0] >> 4;
    if (ver != 6) return -1;

    out->frame       = frame;
    out->frame_len   = len;
    out->tclass      = (uint8_t)(((h[0] & 0x0F) << 4) | (h[1] >> 4));
    out->flow_label  = ((uint32_t)(h[1] & 0x0F) << 16) |
                       ((uint32_t)h[2] << 8) | h[3];
    out->payload_len = get16(h + 4);
    uint8_t next     = h[6];
    out->hop_limit   = h[7];
    for (int i = 0; i < 16; i++) {
        out->src.s6_addr[i] = h[8  + i];
        out->dst.s6_addr[i] = h[24 + i];
    }

    /* Walk extension headers. RFC 8200 §4. */
    int off = eth_off + IPV6_HDR_LEN;
    int avail = len - off;
    if (avail < 0) return -1;
    int payload_remaining = out->payload_len;

    while (next == IPPROTO_HOPOPTS || next == IPPROTO_ROUTING ||
           next == IPPROTO_DSTOPTS || next == IPPROTO_FRAGMENT) {
        if (avail < 8 || payload_remaining < 8) return -1;
        uint8_t this_next = frame[off];
        int ext_len;
        if (next == IPPROTO_FRAGMENT) {
            /* Fragment header is fixed 8 bytes. */
            ext_len = 8;
            /* Drop fragmented packets — minimal stack does not reassemble. */
            return -2;
        } else {
            /* hdr ext len = (frame[off+1] + 1) * 8 */
            ext_len = (frame[off + 1] + 1) * 8;
        }
        if (avail < ext_len || payload_remaining < ext_len) return -1;
        next = this_next;
        off  += ext_len;
        avail -= ext_len;
        payload_remaining -= ext_len;
    }

    out->next_hdr = next;
    out->l4_off   = off;
    out->l4_len   = payload_remaining;
    if (out->l4_len > avail) out->l4_len = avail;
    if (out->l4_len < 0)     out->l4_len = 0;
    return 0;
}

/* ── Header builder ─────────────────────────────── */

void ipv6_build_header(uint8_t *pkt,
                       const uint8_t *dst_mac,
                       const struct in6_addr *src,
                       const struct in6_addr *dst,
                       uint8_t next_hdr, uint16_t plen, uint8_t hop_limit) {
    /* Ethernet */
    mcpy(pkt,     dst_mac,     6);
    mcpy(pkt + 6, net_my_mac,  6);
    put16(pkt + 12, 0x86DD);

    /* IPv6 fixed header at offset 14 */
    uint8_t *h = pkt + 14;
    h[0] = 0x60;        /* Version=6, TC=0 (high) */
    h[1] = 0;           /* TC low + flow high */
    h[2] = 0; h[3] = 0; /* flow low */
    put16(h + 4, plen);
    h[6] = next_hdr;
    h[7] = hop_limit ? hop_limit : IPV6_DEFAULT_HOPLIM;
    for (int i = 0; i < 16; i++) {
        h[8  + i] = src->s6_addr[i];
        h[24 + i] = dst->s6_addr[i];
    }
}

void ipv6_send_frame(const uint8_t *frame, uint16_t len) {
    netif_tx(frame, len);
}

/* ── Source-address selection (RFC 6724 simplified) ── */

int ipv6_select_src(uint32_t ns_id, const struct in6_addr *dst,
                    struct in6_addr *out_src) {
    /* Loopback destinations always source from ::1. */
    if (in6_is_loopback(dst)) {
        in6_copy(out_src, &in6addr_loopback);
        return 0;
    }
    /* Walk the NS's address list; prefer link-local for link-local dst,
     * else any non-loopback. Falls back to ::. */
    int want_ll = in6_is_linklocal(dst);
    uint64_t flags;
    spin_lock_irq(&ipv6_lock, &flags);
    for (ipv6_netif_entry_t *e = ipv6_netif_head; e; e = e->next) {
        if (e->ns->ns_id != ns_id) continue;
        for (ipv6_addr_t *x = e->addrs; x; x = x->next) {
            int ll = in6_is_linklocal(&x->addr);
            int lp = x->is_loopback;
            if (lp) continue;
            if (want_ll && !ll) continue;
            in6_copy(out_src, &x->addr);
            spin_unlock_irq(&ipv6_lock, flags);
            return 0;
        }
    }
    spin_unlock_irq(&ipv6_lock, flags);
    /* No usable address — return :: (unspecified). Callers should treat
     * this as "bind to anything", typical for client sockets that didn't
     * pre-bind. */
    in6_copy(out_src, &in6addr_any);
    return 0;
}

/* ── Dispatcher ─────────────────────────────────── */

void ipv6_input(uint32_t ns_id, const uint8_t *frame, int len) {
    ipv6_pkt_t p;
    if (ipv6_parse(frame, len, 14, &p) != 0) return;

    /* Destination must be one of our addresses (or any-multicast/all-nodes
     * /solicited-node mc). For loopback ::1 and per-iface addrs we accept. */
    if (!in6_is_any(&p.dst) && !ipv6_addr_match_in_ns(ns_id, &p.dst) &&
        !in6_is_loopback(&p.dst) && !in6_is_solicited_node(&p.dst))
        return;

    switch (p.next_hdr) {
    case IPPROTO_TCP_V6:   tcp6_input(ns_id, &p); return;
    case IPPROTO_UDP_V6:   udp6_input(ns_id, &p); return;
    case IPPROTO_ICMPV6:   icmpv6_input(ns_id, &p); return;
    default:               return; /* unknown: silently drop */
    }
}

/* ── Init ───────────────────────────────────────── */

void ipv6_init(void) {
    /* Attach ::1/128 to the init NS loopback. Per-NS loopbacks register
     * theirs lazily on first lookup via ipv6_attach_loopback_for_ns(). */
    struct netif *lo = netif_loopback_ns(&init_net_ns);
    if (lo) ipv6_addr_attach(&init_net_ns, lo, &in6addr_loopback, 128, 1);
    serial_puts("ipv6: init\n");
}

/* Called by net_ns_alloc to install ::1 on the per-NS loopback. */
void ipv6_attach_loopback_for_ns(struct net_ns *ns) {
    struct netif *lo = netif_loopback_ns(ns);
    if (lo) ipv6_addr_attach(ns, lo, &in6addr_loopback, 128, 1);
}

/* Called by ipv6_addr_attach() externally (NDP / SLAAC). */
int ipv6_addr_add(struct net_ns *ns, struct netif *nif,
                  const struct in6_addr *a, uint8_t prefix) {
    return ipv6_addr_attach(ns, nif, a, prefix, 0);
}

/* ── Interface bring-up: SLAAC link-local + DAD ──
 *
 * Called from net_init() after the HW NIC registered. Generates the
 * fe80::<EUI64> link-local for the default interface and tries DAD by
 * sending one Neighbor Solicitation; on a switched LAN the absence of
 * a NA after a tick means the address is unique. We don't block on
 * DAD here — the address is installed optimistically and removed if a
 * conflicting NA arrives later (Linux's "optimistic DAD" mode). */

extern void ndp_make_linklocal(const uint8_t mac[6], struct in6_addr *out);
extern void ndp_send_dad_ns(uint32_t ns_id, const struct in6_addr *target);

void ipv6_iface_bringup(struct netif *nif) {
    if (!nif) return;
    /* Loopback is a special case — ::1 only, no link-local. */
    if (nif->flags & NETIF_F_LOOPBACK) return;

    uint8_t mac[6];
    nif->get_mac(nif, mac);
    /* Skip zero-MAC (uninitialised drivers). */
    int all_zero = 1;
    for (int i = 0; i < 6; i++) if (mac[i]) { all_zero = 0; break; }
    if (all_zero) return;

    struct in6_addr ll;
    ndp_make_linklocal(mac, &ll);
    ipv6_addr_attach(&init_net_ns, nif, &ll, 64, 0);
    ndp_send_dad_ns(init_net_ns.ns_id, &ll);
}
