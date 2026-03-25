# CosmoRT — Offene Punkte

Stand: 2026-03-25. 444 ktest PASS, 0 FAIL.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.
Audit: 27/27 Security-Fixes erledigt.

Netzwerk-Stack v2 Architektur: siehe notes/NETWORK.md

---

## Net Phase 0 — Quick-Fixes (npm unblockieren)

- [x] NET_QUEUE_SIZE 16→128
- [x] NET_TCP_RXBUF 4096→65536
- [x] NET_TCP_TIMEOUT_MS 5000→30000
- [x] TCP recv: timeout → -EAGAIN statt 0 (EOF)
- [x] TCP recv: non-matching Pakete re-queuen statt droppen
- [x] flock(73) Stub
- [x] MAX_SOCKETS 16→64
- [ ] test: npm install -g funktioniert
- [ ] test: claude update funktioniert

## Net Phase A — tcp.c extrahieren

- [x] TCP-Code aus net.c → tcp.c
- [x] Per-Socket Ringbuffer (64KB) statt globale q_tcp
- [x] TCP State-Machine (10 States, RFC 793)
- [x] Hash-Lookup tcp_find(sport, dport, src_ip)
- [x] test/unit/net/test_tcp_state.c
- [x] test/unit/net/test_tcp_rxring.c

## Net Phase B — udp.c extrahieren

- [ ] UDP-Code aus net.c → udp.c
- [ ] Per-Socket Demux statt globale q_udp_sock
- [ ] test/net/test_net_udp_echo.c

## Net Phase C — dispatch.c + arp.c + ip.c

- [ ] Packet-Dispatch aus net.c → dispatch.c
- [ ] ARP-Cache Modul → arp.c
- [ ] IP-Header-Helpers → ip.c
- [ ] test/net/test_arp_cache.c
- [ ] test/net/test_ip_checksum.c

## Net Phase D — dns.c + dhcp.c

- [ ] DNS-Resolver → dns.c
- [ ] mDNS → dns.c
- [ ] DHCP-Client → dhcp.c

## Net Phase E — Robustheit

- [ ] Out-of-Order Segment Buffering
- [ ] Slow-Start + AIMD Congestion Control
- [ ] Non-Blocking TCP Connect (EINPROGRESS)
- [ ] SO_RCVTIMEO / SO_SNDTIMEO pro Socket
- [ ] TCP Keepalive
- [ ] test/net/test_tcp_ooo.c
- [ ] test/net/test_net_multi_conn.c

## Net Tests — End-to-End (gegen echte Server)

- [ ] test/net/test_net_dns.c — DNS gegen QEMU slirp
- [ ] test/net/test_net_tcp_connect.c — TCP zu example.com:80
- [ ] test/net/test_net_tcp_transfer.c — HTTP GET, Body pruefen
- [ ] test/net/test_net_tls.c — HTTPS via Node.js
- [ ] test/net/test_net_npm_registry.c — HTTPS GET registry.npmjs.org

## Offen (nicht Netzwerk)

- [ ] c-ares UDP DNS (c-ares ETIMEOUT trotz korrekter Pakete)
- [ ] Job Control (TIOCSPGRP, SIGTSTP/SIGCONT)
- [ ] Dynamic Linker (ld-cosmo.so — CosmoPX)
- [ ] GPT-Image Boot (Partitions-Support)
