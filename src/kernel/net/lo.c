/* CosmoRT Loopback Interface
 *
 * Registered as netif "lo". Send reinjects packets into RX queues
 * (TCP/UDP/ICMP) without going through a physical NIC.
 */

#include "net/netif.h"
#include "net/net.h"
#include "net/net_util.h"

/* Forward declarations for protocol input */
extern void tcp_input(const uint8_t *pkt, int len);
extern int  udp_input(const uint8_t *pkt, int len);
extern void sched_wake(struct thread *t);
extern void epoll_wake_all(void);

static void lo_send(struct netif *nif, const uint8_t *data, uint16_t len) {
    (void)nif;
    uint8_t buf[1600];
    if (len > 1600) return;
    for (int i = 0; i < len; i++) buf[i] = data[i];

    uint8_t proto = buf[23];
    if (proto == 6) {
        tcp_input(buf, len);
    } else if (proto == 1) {
        q_push(&q_icmp, buf, len);
    } else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(buf + 36);
        if (dport == 68)
            q_push(&q_udp_dhcp, buf, len);
        else if (!udp_input(buf, len))
            q_push(&q_udp_dns, buf, len);
    }
    epoll_wake_all();
}

static uint8_t lo_mac[6] = {0, 0, 0, 0, 0, 0};

static void lo_get_mac(struct netif *nif, uint8_t *out) {
    (void)nif;
    for (int i = 0; i < 6; i++) out[i] = lo_mac[i];
}

static struct netif lo_netif = {
    .name  = "lo",
    .mac   = {0},
    .ip    = {127, 0, 0, 1},
    .flags = NETIF_F_LOOPBACK,
    .mtu   = 65536,
    .send  = lo_send,
    .get_mac = lo_get_mac,
};

void lo_init(void) {
    netif_register(&lo_netif);
}
