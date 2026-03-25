/* CosmoRT DHCP Client — kernel-level DHCP for boot
 * Extracted from net.c (Phase D1).
 */

#include "dhcp.h"
#include "net.h"
#include "net_util.h"
#include "ip.h"
#include "timer.h"

static uint32_t dhcp_saved_xid;

int net_dhcp_check(void) {
    if (net_my_ip[0] != 0) return 1;
    uint8_t reply[Q_PKT];
    int len = q_pop(&q_udp_dhcp, reply, sizeof(reply));
    if (len < 282) return 0;
    if (get16(reply+36) != 68) return 0;
    if (reply[42] != 2) return 0;
    if (get32(reply+46) != dhcp_saved_xid) return 0;

    mcpy(net_my_ip, reply+58, 4);
    int o = 282;
    while (o < len && reply[o] != 255) {
        uint8_t opt = reply[o++];
        if (opt == 0) continue;
        if (o >= len) break;
        uint8_t ol = reply[o++];
        if (o + ol > len) break;
        if (opt == 3 && ol >= 4) mcpy(net_gw_ip, reply+o, 4);
        if (opt == 6 && ol >= 4) mcpy(net_dns_ip, reply+o, 4);
        o += ol;
    }
    return 1;
}

int net_dhcp(void) {
    net_dhcp_send_discover();
    uint64_t deadline = timer_ms() + NET_TCP_TIMEOUT_MS;
    while (timer_ms() < deadline) {
        if (net_dhcp_check()) return 0;
        net_dhcp_send_discover();
        net_idle();
    }
    return -1;
}

void net_dhcp_send_discover(void) {
    uint8_t pkt[590];
    mzero(pkt, sizeof(pkt));
    for (int i = 0; i < 6; i++) pkt[i] = 0xFF;
    mcpy(pkt+6, net_my_mac, 6);
    put16(pkt+12, 0x0800);
    pkt[14] = 0x45; put16(pkt+16, 576-14);
    pkt[22] = 64; pkt[23] = 17;
    for (int i = 0; i < 4; i++) pkt[30+i] = 0xFF;
    uint16_t ic = ip_cksum(pkt+14, 20);
    pkt[24] = (uint8_t)(ic >> 8); pkt[25] = (uint8_t)ic;
    put16(pkt+34, 68); put16(pkt+36, 67);
    put16(pkt+38, 576-14-20);
    pkt[42]=1; pkt[43]=1; pkt[44]=6;
    {
        extern int random_get(void *, unsigned long);
        if (random_get(&dhcp_saved_xid, sizeof(dhcp_saved_xid)) < 0)
            dhcp_saved_xid = (uint32_t)timer_ms();
        put32(pkt+46, dhcp_saved_xid);
    }
    mcpy(pkt+70, net_my_mac, 6);
    pkt[278]=99; pkt[279]=130; pkt[280]=83; pkt[281]=99;
    pkt[282]=53; pkt[283]=1; pkt[284]=1;
    pkt[285]=55; pkt[286]=3; pkt[287]=1; pkt[288]=3; pkt[289]=6;
    pkt[290]=255;
    ip_send_raw(pkt, 590);
}
