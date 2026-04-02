/* CosmoRT Network Interface Abstraction
 *
 * Each NIC (physical or virtual) registers as a struct netif.
 * ip.c routes outgoing packets to the correct interface.
 * Loopback (lo) is a netif that reinjects into RX queues.
 */
#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>

#define NETIF_MAX 4
#define NETIF_NAME_MAX 16

#define NETIF_F_LOOPBACK (1 << 0)
#define NETIF_F_UP       (1 << 1)

struct netif {
    char name[NETIF_NAME_MAX];
    uint8_t mac[6];
    uint8_t ip[4];
    uint32_t flags;
    int mtu;
    void (*send)(struct netif *nif, const uint8_t *data, uint16_t len);
    void (*get_mac)(struct netif *nif, uint8_t *out);
    void *priv;  /* driver-private data */
};

/* Register/deregister a network interface. Returns 0 or -1. */
int netif_register(struct netif *nif);

/* Find interface by name ("lo", "eth0"). Returns NULL if not found. */
struct netif *netif_find(const char *name);

/* Get the default (first non-loopback) interface. */
struct netif *netif_default(void);

/* Get the loopback interface. */
struct netif *netif_loopback(void);

/* Send a raw ethernet frame via the appropriate interface.
 * Routes 127.x.x.x to loopback, everything else to default NIC. */
void netif_tx(const uint8_t *frame, uint16_t len);

/* Number of registered interfaces. */
int netif_count(void);

#endif
