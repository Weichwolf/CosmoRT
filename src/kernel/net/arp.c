/* CosmoRT ARP — Cache + Resolution
 * Extracted from net.c (Phase C).
 */

#include "arp.h"
#include "ip.h"
#include "net.h"
#include "net_util.h"
#include "timer.h"

/* ── ARP Cache ─────────────────────────────────────── */

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t valid;
} arp_entry_t;

static arp_entry_t arp_cache[NET_ARP_CACHE];
static int arp_cache_next;  /* round-robin evict index */

int arp_cache_lookup(const uint8_t *ip, uint8_t *mac_out) {
    for (int i = 0; i < NET_ARP_CACHE; i++) {
        if (arp_cache[i].valid &&
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            mcpy(mac_out, arp_cache[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

void arp_cache_insert(const uint8_t *ip, const uint8_t *mac) {
    /* Update existing entry if present */
    for (int i = 0; i < NET_ARP_CACHE; i++) {
        if (arp_cache[i].valid &&
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            mcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    /* Find free slot */
    for (int i = 0; i < NET_ARP_CACHE; i++) {
        if (!arp_cache[i].valid) {
            mcpy(arp_cache[i].ip, ip, 4);
            mcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Evict round-robin */
    int idx = arp_cache_next % NET_ARP_CACHE;
    mcpy(arp_cache[idx].ip, ip, 4);
    mcpy(arp_cache[idx].mac, mac, 6);
    arp_cache[idx].valid = 1;
    arp_cache_next = idx + 1;
}

void arp_cache_evict(const uint8_t *ip) {
    for (int i = 0; i < NET_ARP_CACHE; i++) {
        if (arp_cache[i].valid &&
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            arp_cache[i].valid = 0;
            return;
        }
    }
}

/* ── ARP Input (from dispatcher) ───────────────────── */

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

/* ── ARP Resolve (blocking) ────────────────────────── */

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
