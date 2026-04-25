/* CosmoRT Network Namespace — alloc/get/put + per-NS loopback bring-up.
 *
 * Layer: net (referenced by proc, sys/proc, fs/procfs, sys/sys_file, socket).
 *
 * The init_net_ns is statically allocated and pinned. Every fresh NS
 * spawned by clone(CLONE_NEWNET)/unshare(CLONE_NEWNET) gets its own
 * loopback interface (127.0.0.1/8) so the in-NS app can still talk to
 * itself; physical NICs stay in init_net_ns.
 */

#include "net/net_ns.h"
#include "net/netif.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "memops.h"
#include "hw/serial.h"
#include "hal/hal_cpu.h"
#include "linux/errno.h"

struct net_ns init_net_ns = {
    .magic            = NET_NS_MAGIC,
    .refcount         = 0x40000000,        /* pinned */
    .ns_id            = 1,                 /* ns_id 1 == root, like Linux */
    .netif_head       = 0,
    .netif_lock       = SPINLOCK_INIT,
    .unix_active_head = 0,
    .unix_lock        = SPINLOCK_INIT,
    .ip_forward       = 0,
    .disable_ipv6     = 0,
};

#define NET_NS_INIT_SLAB_COUNT      4
#define NET_NSFS_HANDLE_INIT_SLAB   8

static slab_t net_ns_slab;
static slab_t net_nsfs_slab;
static spinlock_t net_ns_lock = SPINLOCK_INIT;
static uint32_t  net_ns_next_id = 2; /* 1 reserved for init_net_ns */

/* Per-NS loopback netif backing storage. Allocated alongside net_ns; we
 * piggyback on the slab returned for the NS by reserving the second half
 * of the alloc, but for simplicity we just allocate a separate slab. */
static slab_t lo_netif_slab;

__attribute__((cold, noreturn))
static void net_ns_fail(const char *msg) {
    serial_puts("net_ns: FATAL ");
    serial_puts(msg);
    serial_puts("\n");
    for (;;) hal_cpu_halt_noirq();
}

/* ── Loopback per-NS ──────────────────────────────── */

extern void tcp_input(uint32_t ns_id, const uint8_t *pkt, int len);
extern int  udp_input(uint32_t ns_id, const uint8_t *pkt, int len);
extern void ipv6_input(uint32_t ns_id, const uint8_t *frame, int len);
extern void ipv6_attach_loopback_for_ns(struct net_ns *ns);
extern void route6_attach_lo(struct net_ns *ns);
extern void epoll_wake_all(void);

#include "net/net.h"
#include "net/net_util.h"

static void per_ns_lo_send(struct netif *nif, const uint8_t *data, uint16_t len) {
    uint32_t ns_id = nif->ns ? nif->ns->ns_id : init_net_ns.ns_id;
    uint8_t buf[1600];
    if (len > 1600) return;
    for (int i = 0; i < len; i++) buf[i] = data[i];

    /* IPv6 frame? */
    if (len >= 14 && buf[12] == 0x86 && buf[13] == 0xDD) {
        ipv6_input(ns_id, buf, len);
        epoll_wake_all();
        return;
    }

    uint8_t proto = buf[23];
    if (proto == 6) {
        tcp_input(ns_id, buf, len);
    } else if (proto == 1) {
        q_push(&q_icmp, buf, len);
    } else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(buf + 36);
        if (dport == 68)
            q_push(&q_udp_dhcp, buf, len);
        else if (!udp_input(ns_id, buf, len))
            q_push(&q_udp_dns, buf, len);
    }
    epoll_wake_all();
}

static void per_ns_lo_get_mac(struct netif *nif, uint8_t *out) {
    (void)nif;
    for (int i = 0; i < 6; i++) out[i] = 0;
}

static int per_ns_lo_install(struct net_ns *ns) {
    struct netif *lo = (struct netif *)slab_alloc(&lo_netif_slab);
    if (!lo) return -ENOMEM;
    kmemset(lo, 0, sizeof(*lo));
    lo->name[0]='l'; lo->name[1]='o'; lo->name[2]=0;
    lo->ip[0] = 127; lo->ip[1] = 0; lo->ip[2] = 0; lo->ip[3] = 1;
    lo->flags = NETIF_F_LOOPBACK;
    lo->mtu   = 65536;
    lo->send  = per_ns_lo_send;
    lo->get_mac = per_ns_lo_get_mac;
    return netif_register_ns(ns, lo);
}

static void per_ns_lo_destroy(struct net_ns *ns) {
    struct netif *lo = netif_loopback_ns(ns);
    if (!lo) return;
    netif_unregister_ns(lo);
    slab_free(&lo_netif_slab, lo);
}

/* ── Init / alloc / get / put ─────────────────────── */

__attribute__((cold))
void net_ns_init(void) {
    slab_init_dynamic(&net_ns_slab, (int)sizeof(struct net_ns),
                      NET_NS_INIT_SLAB_COUNT);
    slab_init_dynamic(&net_nsfs_slab, (int)sizeof(struct net_nsfs_handle),
                      NET_NSFS_HANDLE_INIT_SLAB);
    slab_init_dynamic(&lo_netif_slab, (int)sizeof(struct netif), 4);
    serial_puts("net_ns: init\n");
}

struct net_ns *net_ns_alloc(struct net_ns *parent) {
    (void)parent; /* sysctls inherit, but loopback is fresh per NS */
    struct net_ns *ns = (struct net_ns *)slab_alloc(&net_ns_slab);
    if (!ns) return 0;
    kmemset(ns, 0, sizeof(*ns));
    ns->magic    = NET_NS_MAGIC;
    ns->refcount = 1;

    uint64_t f;
    spin_lock_irq(&net_ns_lock, &f);
    ns->ns_id = net_ns_next_id++;
    spin_unlock_irq(&net_ns_lock, f);

    /* sysctls inherited from parent */
    if (parent) {
        ns->ip_forward   = parent->ip_forward;
        ns->disable_ipv6 = parent->disable_ipv6;
    }

    if (per_ns_lo_install(ns) < 0) {
        slab_free(&net_ns_slab, ns);
        return 0;
    }
    ipv6_attach_loopback_for_ns(ns);
    route6_attach_lo(ns);
    return ns;
}

struct net_ns *net_ns_get(struct net_ns *ns) {
    if (!ns) return 0;
    if (ns->magic != NET_NS_MAGIC) net_ns_fail("net_ns_get: bad magic");
    uint64_t f;
    spin_lock_irq(&net_ns_lock, &f);
    ns->refcount++;
    spin_unlock_irq(&net_ns_lock, f);
    return ns;
}

void net_ns_put(struct net_ns *ns) {
    if (!ns || ns == &init_net_ns) return;
    if (ns->magic != NET_NS_MAGIC) net_ns_fail("net_ns_put: bad magic");
    uint64_t f;
    spin_lock_irq(&net_ns_lock, &f);
    int rc = --ns->refcount;
    spin_unlock_irq(&net_ns_lock, f);
    if (rc == 0) {
        per_ns_lo_destroy(ns);
        ns->magic = 0; /* poison */
        slab_free(&net_ns_slab, ns);
    }
}

/* current task's NS, with fallback to init_net_ns for early boot/IRQ ctx
 * where proc_current() returns NULL. The proc_net_ns shim lives in
 * proc/process.c and pulls task->net_ns; declaring it here keeps the
 * net layer header-free of proc internals. The weak default lets early
 * commits land before the proc field exists; once proc.c provides a
 * strong proc_net_ns, the linker prefers it. */
extern struct process *proc_current(void);

__attribute__((weak))
struct net_ns *proc_net_ns(struct process *p) { (void)p; return &init_net_ns; }

struct net_ns *net_ns_current(void) {
    struct process *p = proc_current();
    if (!p) return &init_net_ns;
    struct net_ns *ns = proc_net_ns(p);
    return ns ? ns : &init_net_ns;
}

/* ── nsfs handle ─────────────────────────────────── */

struct net_nsfs_handle *net_nsfs_handle_alloc(struct net_ns *ns) {
    struct net_nsfs_handle *h = (struct net_nsfs_handle *)slab_alloc(&net_nsfs_slab);
    if (!h) return 0;
    h->ns = net_ns_get(ns);
    return h;
}

void net_nsfs_handle_free(struct net_nsfs_handle *h) {
    if (!h) return;
    if (h->ns) net_ns_put(h->ns);
    slab_free(&net_nsfs_slab, h);
}
