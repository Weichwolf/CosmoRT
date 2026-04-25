/* CosmoRT Network Namespace (CLONE_NEWNET, Linux net_namespace).
 *
 * Per-process isolation of: network interfaces, TCP/UDP socket lookup
 * tables, AF_UNIX abstract namespace, ARP cache, IP-stack sysctls.
 *
 * Lifecycle:
 *   init_net_ns   — statically allocated, refcount pinned, hosts every
 *                   physical NIC + the boot-time loopback.
 *   net_ns_alloc  — fresh NS, refcount=1; loopback (lo) auto-created with
 *                   127.0.0.1/8.
 *   net_ns_get    — incref.
 *   net_ns_put    — decref, free on last drop (never frees init_net_ns).
 *
 * Inheritance: fork/clone without CLONE_NEWNET shares the parent NS via
 * incref. CLONE_NEWNET allocates a fresh NS for the child. unshare()
 * replaces current->net_ns. setns(fd) re-binds via /proc/<pid>/ns/net.
 *
 * Hardware NICs (e1000, virtio-net) live only in init_net_ns; explicit
 * `ip link set dev eth0 netns X` migration is out of Phase-15-scope.
 * Loopback is duplicated into every NS, matching Linux behaviour.
 */
#ifndef COSMO_NET_NS_H
#define COSMO_NET_NS_H

#include <stdint.h>
#include "spinlock.h"

#define NET_NS_MAGIC 0x4E45544E53ULL /* 'NETNS' */

struct netif;
struct unix_socket;

struct net_ns {
    uint64_t          magic;          /* NET_NS_MAGIC */
    int               refcount;
    uint32_t          ns_id;          /* monotonic ID since boot, used in /proc/<pid>/ns/net symlink */

    /* Per-NS network interface list. Doubly-linked via netif::ns_next/ns_prev. */
    struct netif     *netif_head;
    spinlock_t        netif_lock;

    /* AF_UNIX abstract namespace head (intrusive ns-list — distinct from
     * the global usock_active_head used for filesystem-walk). Pathname
     * AF_UNIX sockets remain in the global list because they live on the
     * VFS, which is not per-NS in Phase 15. */
    struct unix_socket *unix_active_head;
    spinlock_t          unix_lock;

    /* IP-stack sysctls (/proc/sys/net/ipv4/...). */
    int               ip_forward;
    int               disable_ipv6;
};

/* Boot namespace — pinned, never freed. Hosts the original loopback and
 * every hardware NIC. */
extern struct net_ns init_net_ns;

void                 net_ns_init(void);
struct net_ns       *net_ns_alloc(struct net_ns *parent);
struct net_ns       *net_ns_get(struct net_ns *ns);
void                 net_ns_put(struct net_ns *ns);

/* Helper for hot-path callers: returns current task's net_ns or
 * init_net_ns when no current process exists yet (early boot, IRQ ctx). */
struct net_ns       *net_ns_current(void);

/* nsfs handle (open("/proc/<pid>/ns/net")) — separate from the time_ns
 * variant because setns dispatches on file-op type to nstype. */
struct net_nsfs_handle {
    struct net_ns    *ns;
};

struct net_nsfs_handle *net_nsfs_handle_alloc(struct net_ns *ns);
void                    net_nsfs_handle_free(struct net_nsfs_handle *h);

#endif /* COSMO_NET_NS_H */
