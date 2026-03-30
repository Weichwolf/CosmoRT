/* CosmoRT Network Stack — IP/UDP/TCP/DHCP/ARP/DNS */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "config.h"
#include "spinlock.h"

#include "cosmort.h"

#include "net/tcp.h"

int net_init(void);

const nic_driver_t *net_nic_get(void);

#include "net/dhcp.h"

#include "net/dns.h"

void net_poll(void);

#include "core/rt.h"
rt_channel_t *net_tx_channel(void);

void net_send_raw(const uint8_t *data, uint16_t len);

void net_build_ip_hdr(uint8_t *pkt, const uint8_t *dst_mac,
                      const uint8_t *dst_ip, uint8_t proto, uint16_t plen);

void net_build_ip_hdr_tos(uint8_t *pkt, const uint8_t *dst_mac,
                          const uint8_t *dst_ip, uint8_t proto,
                          uint16_t plen, uint8_t tos);

#define Q_SIZE NET_QUEUE_SIZE
#define Q_PKT  NET_PKT_SIZE
typedef struct {
    uint8_t    data[Q_SIZE][Q_PKT];
    int        len[Q_SIZE];
    int        head, count;
    spinlock_t lock;
} pkt_queue_t;

#define PKT_QUEUE_INIT { .head = 0, .count = 0, .lock = SPINLOCK_INIT }

static inline int q_count(const pkt_queue_t *q) {
    return __atomic_load_n(&q->count, __ATOMIC_ACQUIRE);
}

void q_push(pkt_queue_t *q, const uint8_t *pkt, int len);
int  q_pop(pkt_queue_t *q, uint8_t *buf, int bufsize);

extern pkt_queue_t q_tcp;
extern pkt_queue_t q_udp_dhcp;
extern pkt_queue_t q_udp_dns;
extern pkt_queue_t q_arp;
extern pkt_queue_t q_icmp;

extern struct thread *q_tcp_wait_thread;

extern struct thread *q_arp_wait_thread;

extern struct thread *q_dns_wait_thread;

#include "net/udp.h"

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
