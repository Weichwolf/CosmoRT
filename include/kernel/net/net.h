/* CosmoRT Network Stack — IP/UDP/TCP/DHCP/ARP/DNS
 * NIC-agnostic: drivers register via net_nic_register().
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

/* nic_driver_t, net_nic_register — from cosmo.h (public driver API) */
#include "cosmort.h"

/* TCP types and API */
#include "net/tcp.h"

/* Initialize network state. Requires a NIC to be registered first.
 * Returns 0 on success, -1 if no NIC registered. */
int net_init(void);

/* NIC driver accessor (used by dispatch.c, ip.c) */
const nic_driver_t *net_nic_get(void);

/* DHCP (dhcp.c) */
#include "net/dhcp.h"

/* DNS (dns.c) */
#include "net/dns.h"

/* Polling — call from idle loop and timer interrupt */
void net_poll(void);

/* TX ring channel (Compute→RT). Returns NULL if no NIC registered. */
#include "core/rt.h"
rt_channel_t *net_tx_channel(void);

/* Raw send — compat wrapper, prefer ip_send_raw() */
void net_send_raw(const uint8_t *data, uint16_t len);

/* IP header build — compat wrapper, prefer ip_build_header() */
void net_build_ip_hdr(uint8_t *pkt, const uint8_t *dst_mac,
                      const uint8_t *dst_ip, uint8_t proto, uint16_t plen);

/* IP header build with TOS byte (ECN support) */
void net_build_ip_hdr_tos(uint8_t *pkt, const uint8_t *dst_mac,
                          const uint8_t *dst_ip, uint8_t proto,
                          uint16_t plen, uint8_t tos);

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

/* Queue operations (defined in net.c) */
void q_push(pkt_queue_t *q, const uint8_t *pkt, int len);
int  q_pop(pkt_queue_t *q, uint8_t *buf, int bufsize);

extern pkt_queue_t q_tcp;
extern pkt_queue_t q_udp_dhcp;
extern pkt_queue_t q_udp_dns;
extern pkt_queue_t q_arp;
extern pkt_queue_t q_icmp;

/* Thread blocked on q_tcp (accept/connect handshake), or NULL */
extern struct thread *q_tcp_wait_thread;

/* Thread blocked on ARP reply (resolve), or NULL */
extern struct thread *q_arp_wait_thread;

/* Thread blocked on DNS reply (resolve), or NULL */
extern struct thread *q_dns_wait_thread;

/* UDP types and API (needs pkt_queue_t) */
#include "net/udp.h"

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
