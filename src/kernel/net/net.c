/* CosmoRT Network Stack — IP/UDP/DHCP/ARP/DNS + Dispatch
 * TCP lives in tcp.c. NIC-agnostic: uses registered nic_driver_t.
 */

#include "net.h"
#include "net_util.h"
#include "serial.h"
#include "timer.h"
#include "spinlock.h"
#include "process.h"
#include "percpu.h"
#include "arch.h"
#include "page_alloc.h"

/* Registered NIC driver (set by driver init, e.g., e1000_init) */
static const nic_driver_t *nic;

void net_nic_register(const nic_driver_t *driver) {
    nic = driver;
    if (driver) {
        serial_puts("net: NIC registered: ");
        serial_puts(driver->name);
        serial_putchar('\n');
    } else {
        serial_puts("net: NIC deregistered\n");
    }
}

/* NIC access macros */
#define nic_recv(buf, bufsize)    (nic->recv((buf), (bufsize)))
#define nic_get_mac(mac)          (nic->get_mac((mac)))

/* Network state */
net_state_t net_state = {{0}, {0}, {0}, {0}};

/* mDNS hostname: "cosmo-XXXX" (set after DHCP, max 15 chars) */
static char mdns_hostname[16];
static int  mdns_hostname_len;

/* ── Packet Queues ─────────────────────────────────── */

pkt_queue_t q_tcp      = PKT_QUEUE_INIT;
pkt_queue_t q_udp_dhcp = PKT_QUEUE_INIT;
pkt_queue_t q_udp_dns  = PKT_QUEUE_INIT;
pkt_queue_t q_arp      = PKT_QUEUE_INIT;
pkt_queue_t q_icmp     = PKT_QUEUE_INIT;
static uint16_t dns_local_port = 0;

void q_push(pkt_queue_t *q, const uint8_t *pkt, int len) {
    uint64_t flags;
    spin_lock_irq(&q->lock, &flags);
    if (q->count < Q_SIZE) {
        int idx = (q->head + q->count) % Q_SIZE;
        int l = len > Q_PKT ? Q_PKT : len;
        mcpy(q->data[idx], pkt, l);
        q->len[idx] = l;
        q->count++;
    }
    spin_unlock_irq(&q->lock, flags);
}

int q_pop(pkt_queue_t *q, uint8_t *buf, int bufsize) {
    uint64_t flags;
    spin_lock_irq(&q->lock, &flags);
    if (q->count == 0) {
        spin_unlock_irq(&q->lock, flags);
        return 0;
    }
    int l = q->len[q->head];
    if (l > bufsize) l = bufsize;
    mcpy(buf, q->data[q->head], l);
    q->head = (q->head + 1) % Q_SIZE;
    q->count--;
    spin_unlock_irq(&q->lock, flags);
    return l;
}

/* ── NIC Send (with Loopback) ─────────────────────── */

static void nic_send(const uint8_t *data, uint16_t len) {
    /* Loopback: dst IP 127.x.x.x → feed directly into RX queues */
    if (len >= 34 && data[12] == 0x08 && data[13] == 0x00 && data[30] == 127) {
        uint8_t lo[1600];
        if (len > 1600) return;
        for (int i = 0; i < len; i++) lo[i] = data[i];
        uint8_t proto = lo[23];
        if (proto == 6)       q_push(&q_tcp, lo, len);
        else if (proto == 1)  q_push(&q_icmp, lo, len);
        else if (proto == 17) {
            uint16_t dport = get16(lo + 36);
            if (dport == 68) q_push(&q_udp_dhcp, lo, len);
            else if (!udp_input(lo, len))
                q_push(&q_udp_dns, lo, len);
        }
        return;
    }
    if (nic) nic->send(data, len);
}

/* Public wrappers for tcp.c */
void net_send_raw(const uint8_t *data, uint16_t len) { nic_send(data, len); }

void net_build_ip_hdr(uint8_t *pkt, const uint8_t *dst_mac,
                       const uint8_t *dst_ip, uint8_t proto, uint16_t plen) {
    mcpy(pkt, dst_mac, 6); mcpy(pkt + 6, net_my_mac, 6); put16(pkt + 12, 0x0800);
    pkt[14] = 0x45; pkt[15] = 0; put16(pkt + 16, 20 + plen);
    put16(pkt + 18, 0); put16(pkt + 20, 0x4000);
    pkt[22] = 64; pkt[23] = proto; pkt[24] = 0; pkt[25] = 0;
    mcpy(pkt + 26, net_my_ip, 4); mcpy(pkt + 30, dst_ip, 4);
    uint16_t ic = ip_cksum(pkt + 14, 20);
    pkt[24] = (uint8_t)(ic >> 8); pkt[25] = (uint8_t)ic;
}

/* ── mDNS ──────────────────────────────────────────── */

void net_set_hostname(const char *name) {
    int i = 0;
    while (name[i] && i < 15) { mdns_hostname[i] = name[i]; i++; }
    mdns_hostname[i] = 0;
    mdns_hostname_len = i;
}

/* Check if DNS query name matches "cosmo-XXXX.local" (our hostname).
 * qname starts at dns+offset, returns 1 if match. */
static int mdns_name_match(const uint8_t *dns, int offset, int dns_len) {
    if (!mdns_hostname_len || offset >= dns_len) return 0;
    /* Expect: <len>cosmo-XXXX<5>local<0> */
    int pos = offset;
    if (pos >= dns_len) return 0;
    int label1_len = dns[pos++];
    if (label1_len != mdns_hostname_len) return 0;
    if (pos + label1_len > dns_len) return 0;
    for (int i = 0; i < label1_len; i++) {
        char c = (char)dns[pos + i];
        /* Case-insensitive compare */
        if (c >= 'A' && c <= 'Z') c += 32;
        char h = mdns_hostname[i];
        if (h >= 'A' && h <= 'Z') h += 32;
        if (c != h) return 0;
    }
    pos += label1_len;
    if (pos >= dns_len) return 0;
    /* "local" label */
    if (dns[pos] != 5) return 0;
    pos++;
    if (pos + 5 > dns_len) return 0;
    const char *loc = "local";
    for (int i = 0; i < 5; i++) {
        char c = (char)dns[pos + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != loc[i]) return 0;
    }
    pos += 5;
    if (pos >= dns_len || dns[pos] != 0) return 0;
    return 1;
}

/* Send mDNS response: our hostname → our IP */
static void mdns_respond(const uint8_t *src_mac, const uint8_t *src_ip) {
    if (net_my_ip[0] == 0) return;  /* no IP yet */

    /* Build: Ethernet + IP + UDP + DNS response */
    uint8_t pkt[256];
    mzero(pkt, sizeof(pkt));

    /* Ethernet: reply to sender */
    mcpy(pkt, src_mac, 6);
    mcpy(pkt + 6, net_my_mac, 6);
    put16(pkt + 12, 0x0800);

    /* Build DNS answer first to know length */
    uint8_t *dns = pkt + 42;
    put16(dns, 0);            /* ID = 0 (mDNS) */
    put16(dns + 2, 0x8400);   /* QR=1, AA=1 */
    put16(dns + 4, 0);        /* QDCOUNT = 0 */
    put16(dns + 6, 1);        /* ANCOUNT = 1 */
    put16(dns + 8, 0);        /* NSCOUNT = 0 */
    put16(dns + 10, 0);       /* ARCOUNT = 0 */

    int pos = 12;
    /* Name: <len>hostname<5>local<0> */
    dns[pos++] = (uint8_t)mdns_hostname_len;
    for (int i = 0; i < mdns_hostname_len; i++)
        dns[pos++] = (uint8_t)mdns_hostname[i];
    dns[pos++] = 5;
    dns[pos++] = 'l'; dns[pos++] = 'o'; dns[pos++] = 'c';
    dns[pos++] = 'a'; dns[pos++] = 'l';
    dns[pos++] = 0;

    put16(dns + pos, 1);      /* TYPE = A */
    pos += 2;
    put16(dns + pos, 0x8001); /* CLASS = IN, cache-flush bit */
    pos += 2;
    put32(dns + pos, 120);    /* TTL = 120s */
    pos += 4;
    put16(dns + pos, 4);      /* RDLENGTH = 4 */
    pos += 2;
    mcpy(dns + pos, net_my_ip, 4);
    pos += 4;

    int dns_len = pos;
    int udp_len = 8 + dns_len;
    int ip_len = 20 + udp_len;

    /* IP header */
    pkt[14] = 0x45;
    put16(pkt + 16, (uint16_t)ip_len);
    pkt[22] = 64;  /* TTL */
    pkt[23] = 17;  /* UDP */
    mcpy(pkt + 26, net_my_ip, 4);
    mcpy(pkt + 30, src_ip, 4);
    uint16_t ic = ip_cksum(pkt + 14, 20);
    pkt[24] = (uint8_t)(ic >> 8);
    pkt[25] = (uint8_t)ic;

    /* UDP header */
    put16(pkt + 34, 5353);     /* src port */
    put16(pkt + 36, 5353);     /* dst port */
    put16(pkt + 38, (uint16_t)udp_len);

    nic_send(pkt, (uint16_t)(14 + ip_len));
}

/* Handle incoming mDNS query packet (full Ethernet frame) */
static void mdns_handle(const uint8_t *pkt, int len) {
    if (!mdns_hostname_len) return;
    if (len < 42 + 12) return;  /* too short for DNS header */
    const uint8_t *dns = pkt + 42;
    int dns_len = len - 42;
    uint16_t flags = get16(dns + 2);
    if (flags & 0x8000) return;  /* response, not query */
    int qdcount = get16(dns + 4);
    if (qdcount < 1) return;

    /* Check first question */
    if (mdns_name_match(dns, 12, dns_len)) {
        mdns_respond(pkt + 6, pkt + 26);  /* src MAC, src IP */
    }
}

/* ── Central Packet Dispatcher ─────────────────────── */

static volatile int net_poll_active;

void net_poll(void) {
    if (!nic) return;
    /* Reentrancy guard: timer IRQ can fire during net_poll */
    if (__sync_lock_test_and_set(&net_poll_active, 1)) return;

    uint8_t pkt[Q_PKT];
    int len = nic_recv(pkt, sizeof(pkt));
    if (len < 14) goto out;
    uint16_t etype = get16(pkt + 12);
    if (etype == 0x0806) { if (len >= 42) q_push(&q_arp, pkt, len); goto out; }
    if (etype != 0x0800 || len < 34) goto out;

    int ihl = (pkt[14] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl > len) goto out;
    /* IP checksum skipped — QEMU may use NIC checksum offloading */
    int ip_total = get16(pkt + 16);
    if (ip_total < ihl || 14 + ip_total > len) goto out;

    uint8_t proto = pkt[23];
    int queued = 0;
    if (proto == 6)       { q_push(&q_tcp, pkt, len); queued = 1; }
    else if (proto == 1)  { q_push(&q_icmp, pkt, len); queued = 1; }
    else if (proto == 17 && len >= 42) {
        uint16_t dport = get16(pkt+36);
        if (dport == 68)
            q_push(&q_udp_dhcp, pkt, len);
        else if (dport == 5353)
            mdns_handle(pkt, len);
        else if (dns_local_port && dport == dns_local_port)
            q_push(&q_udp_dns, pkt, len);
        else {
            udp_input(pkt, len);
        }
        queued = 1;
    }

    /* Wake epoll sleepers when new packets arrive */
    if (queued) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }

out:
    __sync_lock_release(&net_poll_active);
}

/* poll_n removed: E1000 is IRQ-driven, net_poll() called from IRQ handler.
 * Callers use net_idle() (sti;hlt) to sleep until next interrupt. */

/* ── Init ──────────────────────────────────────────── */

int net_init(void) {
    if (!nic) return -1;  /* no NIC driver registered */
    nic_get_mac(net_my_mac);
    return 0;
}

/* ── DHCP ──────────────────────────────────────────── */
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
        /* Re-send discover every ~500ms in case first was lost */
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
    nic_send(pkt, 590);
}

/* ── ARP ───────────────────────────────────────────── */

int net_arp_resolve(const uint8_t *ip, uint8_t *mac_out) {
    uint8_t pkt[42];
    mzero(pkt, 42);
    for (int i = 0; i < 6; i++) pkt[i] = 0xFF;
    mcpy(pkt+6, net_my_mac, 6);
    put16(pkt+12, 0x0806);
    put16(pkt+14, 1); put16(pkt+16, 0x0800);
    pkt[18]=6; pkt[19]=4; put16(pkt+20, 1);
    mcpy(pkt+22, net_my_mac, 6);
    mcpy(pkt+28, net_my_ip, 4);
    mcpy(pkt+38, ip, 4);
    nic_send(pkt, 42);

    uint8_t reply[Q_PKT];
    uint64_t deadline = timer_ms() + NET_DHCP_RETRY_MS;
    while (timer_ms() < deadline) {
        int len = q_pop(&q_arp, reply, sizeof(reply));
        if (len < 42) { net_idle(); continue; }
        if (get16(reply+20) != 2) continue;
        if (reply[22]==0xFF && reply[23]==0xFF && reply[24]==0xFF &&
            reply[25]==0xFF && reply[26]==0xFF && reply[27]==0xFF) continue;
        if (reply[22]==0 && reply[23]==0 && reply[24]==0 &&
            reply[25]==0 && reply[26]==0 && reply[27]==0) continue;
        if (reply[38]!=net_my_ip[0] || reply[39]!=net_my_ip[1] ||
            reply[40]!=net_my_ip[2] || reply[41]!=net_my_ip[3]) continue;
        if (reply[28]==ip[0] && reply[29]==ip[1] &&
            reply[30]==ip[2] && reply[31]==ip[3]) {
            mcpy(mac_out, reply+22, 6);
            return 0;
        }
    }
    return -1;
}

/* ── Ping ──────────────────────────────────────────── */

int net_ping(const uint8_t *dst_ip) {
    uint8_t gw_mac[6];
    if (net_arp_resolve(net_gw_ip, gw_mac) < 0) return -1;

    uint8_t pkt[98];
    mzero(pkt, 98);
    mcpy(pkt, gw_mac, 6); mcpy(pkt+6, net_my_mac, 6);
    put16(pkt+12, 0x0800);
    pkt[14]=0x45; put16(pkt+16, 84); pkt[22]=64; pkt[23]=1;
    mcpy(pkt+26, net_my_ip, 4); mcpy(pkt+30, dst_ip, 4);
    uint16_t ic = ip_cksum(pkt+14, 20);
    pkt[24]=(uint8_t)(ic>>8); pkt[25]=(uint8_t)ic;
    pkt[34]=8; put16(pkt+38, 0x1234); put16(pkt+40, 1);
    for (int i = 0; i < 56; i++) pkt[42+i] = (uint8_t)i;
    uint16_t ick = ip_cksum(pkt+34, 64);
    pkt[36]=(uint8_t)(ick>>8); pkt[37]=(uint8_t)ick;
    nic_send(pkt, 98);

    uint8_t reply[Q_PKT];
    uint64_t deadline = timer_ms() + NET_DHCP_RETRY_MS;
    while (timer_ms() < deadline) {
        int len = q_pop(&q_icmp, reply, sizeof(reply));
        if (len < 42) { net_idle(); continue; }
        if (reply[34] != 0) continue;
        if (get16(reply+38) != 0x1234) continue;
        return reply[22];
    }
    return -1;
}

/* ── HTTP GET ──────────────────────────────────────── */

int net_http_get(const uint8_t *dst_ip, uint16_t port,
                 const char *host, const char *path,
                 char *response, int resp_size) {
    net_tcp_t conn; mzero(&conn, sizeof(conn));
    if (net_tcp_connect(&conn, dst_ip, port) < 0) return -1;
    char req[512]; int ri = 0; const char *s;
    for (s="GET "; *s && ri < 510;) req[ri++]=*s++;
    for (s=path; *s && ri < 510;) req[ri++]=*s++;
    for (s=" HTTP/1.0\r\nHost: "; *s && ri < 510;) req[ri++]=*s++;
    for (s=host; *s && ri < 510;) req[ri++]=*s++;
    for (s="\r\nConnection: close\r\n\r\n"; *s && ri < 510;) req[ri++]=*s++;
    if (ri >= 510) { net_tcp_close(&conn); return -1; }
    net_tcp_send(&conn, req, ri);
    int total = net_tcp_recv(&conn, response, resp_size-1, 10000);
    if (total > 0) response[total] = 0; else response[0] = 0;
    net_tcp_close(&conn);
    return total;
}

/* ── DNS ───────────────────────────────────────────── */

int net_dns_resolve(const char *hostname, uint8_t ip_out[4]) {
    if (net_dns_ip[0] == 0) {
        if (net_gw_ip[0]) mcpy(net_dns_ip, net_gw_ip, 4);
        else return -1;
    }

    uint8_t gw_mac[6];
    if (net_arp_resolve(net_gw_ip, gw_mac) < 0) return -1;

    {
        extern int random_get(void *, unsigned long);
        uint16_t rnd16;
        if (random_get(&rnd16, sizeof(rnd16)) < 0)
            rnd16 = (uint16_t)(timer_ms() & 0xFFFF);
        dns_local_port = (uint16_t)(49152 + (rnd16 & 0x3FFF));
    }

    uint8_t pkt[256]; mzero(pkt, sizeof(pkt));
    mcpy(pkt, gw_mac, 6); mcpy(pkt+6, net_my_mac, 6);
    put16(pkt+12, 0x0800);
    pkt[14] = 0x45; pkt[22] = 64; pkt[23] = 17;
    mcpy(pkt+26, net_my_ip, 4);
    mcpy(pkt+30, net_dns_ip, 4);
    put16(pkt+34, dns_local_port); put16(pkt+36, 53);

    uint8_t *dns = pkt + 42;
    uint16_t txid;
    {
        extern int random_get(void *, unsigned long);
        if (random_get(&txid, sizeof(txid)) < 0)
            txid = (uint16_t)(timer_ms() & 0xFFFF);
    }
    put16(dns, txid);
    put16(dns+2, 0x0100);
    put16(dns+4, 1);
    put16(dns+6, 0); put16(dns+8, 0); put16(dns+10, 0);

    int qi = 12;
    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - p);
        if (label_len > 63 || qi + label_len + 1 > 200) return -1;
        dns[qi++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) dns[qi++] = (uint8_t)p[i];
        p = *dot ? dot + 1 : dot;
    }
    dns[qi++] = 0;
    put16(dns+qi, 1); qi += 2;
    put16(dns+qi, 1); qi += 2;

    int dns_len = qi;
    int udp_len = 8 + dns_len;
    int ip_len = 20 + udp_len;
    put16(pkt+16, (uint16_t)ip_len);
    put16(pkt+38, (uint16_t)udp_len);
    uint16_t ic = ip_cksum(pkt+14, 20);
    pkt[24] = (uint8_t)(ic >> 8); pkt[25] = (uint8_t)ic;

    nic_send(pkt, (uint16_t)(14 + ip_len));

    uint8_t reply[Q_PKT];
    uint64_t deadline = timer_ms() + NET_DHCP_RETRY_MS;
    while (timer_ms() < deadline) {
        int len = q_pop(&q_udp_dns, reply, sizeof(reply));
        if (len < 42 + 12) { net_idle(); continue; }

        uint8_t *rdns = reply + 42;
        if (get16(rdns) != txid) continue;
        uint16_t flags = get16(rdns+2);
        if (!(flags & 0x8000)) continue;
        if ((flags & 0x000F) != 0) { dns_local_port = 0; return -1; }

        int ancount = get16(rdns+6);
        int ri2 = 12;
        int rdns_len = len - 42;
        while (ri2 < rdns_len && rdns[ri2] != 0) {
            if ((rdns[ri2] & 0xC0) == 0xC0) { ri2 += 2; break; }
            int lbl = rdns[ri2] + 1;
            if (ri2 + lbl > rdns_len) break;
            ri2 += lbl;
        }
        if (ri2 < rdns_len && rdns[ri2] == 0) ri2++;
        if (ri2 + 4 > rdns_len) continue;
        ri2 += 4;

        for (int a = 0; a < ancount && ri2 + 12 <= rdns_len; a++) {
            if ((rdns[ri2] & 0xC0) == 0xC0) { ri2 += 2; if (ri2 > rdns_len) break; }
            else { while (ri2 < rdns_len && rdns[ri2]) { int l = rdns[ri2]+1; if (ri2+l > rdns_len) break; ri2 += l; } if (ri2 >= rdns_len) break; ri2++; }
            if (ri2 + 10 > rdns_len) break;
            uint16_t rtype = get16(rdns+ri2); ri2 += 2;
            ri2 += 2; ri2 += 4;
            uint16_t rdlen = get16(rdns+ri2); ri2 += 2;
            if (rtype == 1 && rdlen == 4 && ri2 + 4 <= rdns_len) {
                mcpy(ip_out, rdns+ri2, 4);
                dns_local_port = 0;
                return 0;
            }
            ri2 += rdlen;
        }
    }
    dns_local_port = 0;
    return -1;
}

