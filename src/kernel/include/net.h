/* CosmoRT Network Stack — IP/UDP/TCP/DHCP/ARP/DNS
 * NIC-agnostic: drivers register via net_nic_register().
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

/* ── NIC driver interface (implemented by each driver) ── */

typedef struct {
    int  (*send)(const void *data, uint16_t len);
    int  (*recv)(void *buf, uint16_t bufsize);
    void (*get_mac)(uint8_t mac[6]);
    const char *name;  /* "e1000", "virtio-net", etc. */
} nic_driver_t;

/* Register a NIC driver. Called by driver init (e.g., e1000_init). */
void net_nic_register(const nic_driver_t *nic);

/* Initialize network state. Requires a NIC to be registered first.
 * Returns 0 on success, -1 if no NIC registered. */
int net_init(void);

/* DHCP */
void net_dhcp_send_discover(void);
int net_dhcp_check(void);
int net_dhcp(void);

/* Ping */
int net_ping(const uint8_t *dst_ip);

/* ARP */
int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

/* TCP */
typedef struct {
    uint8_t  dst_ip[4];
    uint8_t  dst_mac[6];
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t seq;
    uint32_t ack;
    int      state;
    uint8_t  rxbuf[NET_TCP_RXBUF];
    int      rxbuf_pos;
    int      rxbuf_len;
} net_tcp_t;

int net_tcp_connect(net_tcp_t *c, const uint8_t *dst_ip, uint16_t port);
int net_tcp_send(net_tcp_t *c, const void *data, int len);
int net_tcp_recv(net_tcp_t *c, void *buf, int bufsize, int timeout_iter);
void net_tcp_close(net_tcp_t *c);

/* HTTP */
int net_http_get(const uint8_t *dst_ip, uint16_t port,
                 const char *host, const char *path,
                 char *response, int resp_size);

/* DNS */
int net_dns_resolve(const char *hostname, uint8_t ip_out[4]);

/* Polling — call from idle loop and timer interrupt */
void net_poll(void);

/* mDNS hostname (e.g., "cosmo-3456") — set after DHCP */
void net_set_hostname(const char *name);

/* Queue type — each queue has its own spinlock.
 *
 * Lock ordering (lower number = outer lock):
 *   1. No queue lock nests inside another queue lock.
 *   2. q_push/q_pop each take exactly one queue lock, never two.
 *   3. net_poll dispatches to one queue per packet — no cross-queue lock.
 *   4. IRQ-safe: spin_lock_irq (cli before lock, restore after unlock).
 *
 * Deadlock-free by construction: single-lock operations only.
 */
#define Q_SIZE NET_QUEUE_SIZE
#define Q_PKT  NET_PKT_SIZE
typedef struct {
    uint8_t    data[Q_SIZE][Q_PKT];
    int        len[Q_SIZE];
    int        head, count;
    spinlock_t lock;
} pkt_queue_t;

#define PKT_QUEUE_INIT { .head = 0, .count = 0, .lock = SPINLOCK_INIT }

/* Atomic read of queue packet count (safe without lock for polling hints) */
static inline int q_count(const pkt_queue_t *q) {
    return __atomic_load_n(&q->count, __ATOMIC_ACQUIRE);
}

extern pkt_queue_t q_tcp;

/* Network state */
typedef struct {
    uint8_t my_ip[4];
    uint8_t my_mac[6];
    uint8_t gw_ip[4];
    uint8_t dns_ip[4];
} net_state_t;
extern net_state_t net_state;
#define net_my_ip   net_state.my_ip
#define net_my_mac  net_state.my_mac
#define net_gw_ip   net_state.gw_ip
#define net_dns_ip  net_state.dns_ip

#endif
