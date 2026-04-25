/* CosmoRT Network Interface Abstraction
 *
 * Each NIC (physical or virtual) registers as a struct netif.
 * ip.c routes outgoing packets to the correct interface.
 * Loopback (lo) is a netif that reinjects into RX queues.
 */
#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>

#define NETIF_NAME_MAX 16

#define NETIF_F_LOOPBACK (1 << 0)
#define NETIF_F_UP       (1 << 1)

struct net_ns;

struct netif {
    char name[NETIF_NAME_MAX];
    uint8_t mac[6];
    uint8_t ip[4];
    uint32_t flags;
    int mtu;
    void (*send)(struct netif *nif, const uint8_t *data, uint16_t len);
    void (*get_mac)(struct netif *nif, uint8_t *out);
    void *priv;  /* driver-private data */

    /* Per-NS list linkage. Set by netif_register_ns; init_net_ns owns
     * physical NICs, every other NS gets its own loopback. */
    struct net_ns *ns;
    struct netif  *ns_next;
    struct netif  *ns_prev;
};

/* Register an interface in init_net_ns (back-compat for HW NIC drivers). */
int netif_register(struct netif *nif);

/* Register an interface in an explicit NS. ns==NULL → init_net_ns. */
int netif_register_ns(struct net_ns *ns, struct netif *nif);

/* Detach from its NS list and clear ns/ns_next/ns_prev. Caller still owns
 * the struct's storage; used when an NS is torn down. */
void netif_unregister_ns(struct netif *nif);

/* Find interface by name in the *current task's* NS. */
struct netif *netif_find(const char *name);

/* Find interface by name in an explicit NS. ns==NULL → init_net_ns. */
struct netif *netif_find_ns(struct net_ns *ns, const char *name);

/* Get the default (first non-loopback) interface in current NS. */
struct netif *netif_default(void);
struct netif *netif_default_ns(struct net_ns *ns);

/* Get the loopback interface for current NS. */
struct netif *netif_loopback(void);
struct netif *netif_loopback_ns(struct net_ns *ns);

/* Send a raw ethernet frame via the appropriate interface in current NS.
 * Routes 127.x.x.x to loopback, everything else to default NIC. */
void netif_tx(const uint8_t *frame, uint16_t len);

/* Number of registered interfaces in init_net_ns (legacy stat). */
int netif_count(void);

#endif
