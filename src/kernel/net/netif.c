/* CosmoRT Network Interface — registration, routing, send */

#include "net/netif.h"
#include "hw/serial.h"

static struct netif *interfaces[NETIF_MAX];
static int nif_count;

int netif_register(struct netif *nif) {
    if (!nif || nif_count >= NETIF_MAX) return -1;
    interfaces[nif_count++] = nif;
    nif->flags |= NETIF_F_UP;
    serial_puts("netif: ");
    serial_puts(nif->name);
    serial_puts(" up\n");
    return 0;
}

struct netif *netif_find(const char *name) {
    for (int i = 0; i < nif_count; i++) {
        const char *a = interfaces[i]->name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return interfaces[i];
    }
    return 0;
}

struct netif *netif_default(void) {
    for (int i = 0; i < nif_count; i++)
        if (!(interfaces[i]->flags & NETIF_F_LOOPBACK))
            return interfaces[i];
    return 0;
}

struct netif *netif_loopback(void) {
    for (int i = 0; i < nif_count; i++)
        if (interfaces[i]->flags & NETIF_F_LOOPBACK)
            return interfaces[i];
    return 0;
}

void netif_tx(const uint8_t *frame, uint16_t len) {
    /* Route: 127.x.x.x → loopback, else → default NIC */
    if (len >= 34 && frame[12] == 0x08 && frame[13] == 0x00 && frame[30] == 127) {
        struct netif *lo = netif_loopback();
        if (lo && lo->send) { lo->send(lo, frame, len); return; }
    }
    struct netif *def = netif_default();
    if (def && def->send) def->send(def, frame, len);
}

int netif_count(void) { return nif_count; }
