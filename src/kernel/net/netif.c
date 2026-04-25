/* CosmoRT Network Interface — registration, routing, send.
 * Per-NS linked list (struct netif::ns_next/ns_prev). Hardware NICs live
 * in init_net_ns; every NS additionally owns its own loopback. */

#include "net/netif.h"
#include "net/net_ns.h"
#include "hw/serial.h"

int netif_register_ns(struct net_ns *ns, struct netif *nif) {
    if (!nif) return -1;
    if (!ns) ns = &init_net_ns;

    uint64_t flags;
    spin_lock_irq(&ns->netif_lock, &flags);

    nif->ns = ns;
    nif->ns_prev = 0;
    nif->ns_next = ns->netif_head;
    if (ns->netif_head) ns->netif_head->ns_prev = nif;
    ns->netif_head = nif;
    nif->flags |= NETIF_F_UP;

    spin_unlock_irq(&ns->netif_lock, flags);

    if (ns == &init_net_ns) {
        serial_puts("netif: ");
        serial_puts(nif->name);
        serial_puts(" up\n");
    }
    return 0;
}

int netif_register(struct netif *nif) {
    return netif_register_ns(&init_net_ns, nif);
}

void netif_unregister_ns(struct netif *nif) {
    if (!nif || !nif->ns) return;
    struct net_ns *ns = nif->ns;
    uint64_t flags;
    spin_lock_irq(&ns->netif_lock, &flags);
    if (nif->ns_prev) nif->ns_prev->ns_next = nif->ns_next;
    else              ns->netif_head        = nif->ns_next;
    if (nif->ns_next) nif->ns_next->ns_prev = nif->ns_prev;
    nif->ns_next = 0;
    nif->ns_prev = 0;
    nif->ns = 0;
    spin_unlock_irq(&ns->netif_lock, flags);
}

struct netif *netif_find_ns(struct net_ns *ns, const char *name) {
    if (!ns) ns = &init_net_ns;
    uint64_t flags;
    spin_lock_irq(&ns->netif_lock, &flags);
    struct netif *found = 0;
    for (struct netif *n = ns->netif_head; n; n = n->ns_next) {
        const char *a = n->name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) { found = n; break; }
    }
    spin_unlock_irq(&ns->netif_lock, flags);
    return found;
}

struct netif *netif_find(const char *name) {
    return netif_find_ns(net_ns_current(), name);
}

struct netif *netif_default_ns(struct net_ns *ns) {
    if (!ns) ns = &init_net_ns;
    uint64_t flags;
    spin_lock_irq(&ns->netif_lock, &flags);
    struct netif *found = 0;
    for (struct netif *n = ns->netif_head; n; n = n->ns_next) {
        if (!(n->flags & NETIF_F_LOOPBACK)) { found = n; break; }
    }
    spin_unlock_irq(&ns->netif_lock, flags);
    return found;
}

struct netif *netif_default(void) {
    return netif_default_ns(net_ns_current());
}

struct netif *netif_loopback_ns(struct net_ns *ns) {
    if (!ns) ns = &init_net_ns;
    uint64_t flags;
    spin_lock_irq(&ns->netif_lock, &flags);
    struct netif *found = 0;
    for (struct netif *n = ns->netif_head; n; n = n->ns_next) {
        if (n->flags & NETIF_F_LOOPBACK) { found = n; break; }
    }
    spin_unlock_irq(&ns->netif_lock, flags);
    return found;
}

struct netif *netif_loopback(void) {
    return netif_loopback_ns(net_ns_current());
}

void netif_tx(const uint8_t *frame, uint16_t len) {
    /* Route: 127.x.x.x → current NS's loopback, else → current NS's default NIC.
     * Hardware NICs live only in init_net_ns; non-init NS without a default
     * NIC silently drops non-loopback frames (matches Linux netns isolation). */
    struct net_ns *ns = net_ns_current();
    if (len >= 34 && frame[12] == 0x08 && frame[13] == 0x00 && frame[30] == 127) {
        struct netif *lo = netif_loopback_ns(ns);
        if (lo && lo->send) { lo->send(lo, frame, len); return; }
    }
    struct netif *def = netif_default_ns(ns);
    if (def && def->send) def->send(def, frame, len);
}

int netif_count(void) {
    int n = 0;
    uint64_t flags;
    spin_lock_irq(&init_net_ns.netif_lock, &flags);
    for (struct netif *p = init_net_ns.netif_head; p; p = p->ns_next) n++;
    spin_unlock_irq(&init_net_ns.netif_lock, flags);
    return n;
}
