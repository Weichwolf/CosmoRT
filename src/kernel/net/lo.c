/* CosmoRT Loopback Interface
 *
 * Registered as netif "lo". Send reinjects packets into RX queues
 * (TCP/UDP/ICMP) without going through a physical NIC.
 */

#include "net/netif.h"
#include "net/net.h"
#include "net/net_util.h"

/* Forward declarations for protocol input */
#include "net/net_ns.h"
extern void tcp_input(uint32_t ns_id, const uint8_t *pkt, int len);
extern int  udp_input(uint32_t ns_id, const uint8_t *pkt, int len);
extern void ipv6_input(uint32_t ns_id, const uint8_t *frame, int len);
extern void sched_wake(struct thread *t);

/* The init_net_ns loopback. Per-NS loopback uses the parallel send
 * function defined in net_ns.c, which keys input dispatch on the
 * sender's NS rather than always init. Protocol input wakes per-socket
 * wqs (tcp.wait_wq / udp.recv_wq) which forward to ep->wq via
 * ep_poll_callback for any registered epitem. */
static void lo_send(struct netif *nif, const uint8_t *data, uint16_t len) {
    (void)nif;
    uint8_t buf[1600];
    if (len > 1600) return;
    for (int i = 0; i < len; i++) buf[i] = data[i];

    uint32_t ns = init_net_ns.ns_id;

    if (len >= 14 && buf[12] == 0x86 && buf[13] == 0xDD) {
        ipv6_input(ns, buf, len);
        return;
    }

    uint8_t proto = buf[23];
    if (proto == 6) {
        tcp_input(ns, buf, len);
    } else if (proto == 1) {
        q_push(&q_icmp, buf, len);
    } else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(buf + 36);
        if (dport == 68)
            q_push(&q_udp_dhcp, buf, len);
        else if (!udp_input(ns, buf, len))
            q_push(&q_udp_dns, buf, len);
    }
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
