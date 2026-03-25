# CosmoRT Netzwerk-Stack v2

## Status quo

914 Zeilen `net.c` = ARP + DHCP + DNS + mDNS + TCP + UDP + HTTP + IP + Dispatch.
Globale Queues (`q_tcp`, `q_udp_sock`) fuer ALLE Verbindungen.
Blocking-only TCP. Kein State-Machine. Kein Flow-Control.

Kern-Probleme:
- 1 globale `q_tcp` → Pakete werden falschem Socket zugestellt/verworfen
- `NET_QUEUE_SIZE=16` → TLS-Handshake ueberlaeuft Queue
- `NET_TCP_RXBUF=4096` → 1 TLS-Record = 16KB, passt nicht
- Blocking TCP connect/recv → Node.js Event-Loop blockiert
- Out-of-Order → Drop → Datenverlust bei Internet-Reordering
- Timeout 0 = EOF → Node.js denkt Connection ist zu

## Ziel-Architektur

```
src/kernel/net/
  dispatch.c     NIC → IP → Demux nach Protokoll+Port
  arp.c          ARP Cache + Resolution
  ip.c           IP Checksum, Header-Build, Loopback
  tcp.c          TCP State-Machine, Per-Socket Ringbuffer
  udp.c          UDP Send/Recv, Per-Socket Demux
  dns.c          Kernel DNS Resolver + mDNS
  dhcp.c         DHCP Client
  socket.c       BSD Socket API (Syscall-Interface)
  unix_socket.c  Unix Domain Sockets
```

## Design-Prinzipien

### Per-Socket Ringbuffer (statt globale Queue)

Linux: `sk_buff` Linked-Lists mit malloc pro Paket → GC-Pressure, Cache-Misses.
CosmoRT: Statischer Ringbuffer pro Socket. Keine Allokation im Hot-Path.

```c
typedef struct {
    uint8_t  buf[NET_TCP_RXBUF];  /* 64KB Ringbuffer */
    uint32_t head, tail;           /* Byte-Positionen */
    spinlock_t lock;
} tcp_rxring_t;
```

### Demux am Eingang

Pakete werden bei Empfang sofort dem richtigen Socket zugeordnet
(Hash-Lookup O(1)), nicht erst beim recv-Syscall aus einer globalen
Queue gefischt.

```
NIC IRQ → net_poll → dispatch → tcp_input → tcp_find(ports) → rxring_push
                                 udp_input → udp_find(port) → per-socket queue
```

### TCP State-Machine (RFC 793, vereinfacht)

```
CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT1 → FIN_WAIT2 → TIME_WAIT → CLOSED
                     ESTABLISHED → CLOSE_WAIT → LAST_ACK → CLOSED
```

10 States. Slow-Start + AIMD. Kein SACK (Phase E: optional).
Muss auf Bare-Metal, VMs und jeder Virtualisierung korrekt funktionieren.

### Wo CosmoRT besser als Linux

| Aspekt | Linux | CosmoRT v2 |
|--------|-------|------------|
| Paket-Allokation | sk_buff malloc pro Paket | Statischer Ringbuffer, Zero-Alloc |
| Receive-Path | NIC→NAPI→GRO→Protocol→Socket→Backlog→User | NIC→Dispatch→Socket-Ring→User |
| Locking | bh_lock_sock + sock_lock + sk_backlog | 1 Spinlock pro Socket, IRQ-safe |
| Overhead | Netfilter immer aktiv | Kein Netfilter |
| Core-Affinity | RSS/RPS/RFS (komplex) | RT-Core empfaengt, Compute liest |

### TCP Connection Struct (v2)

```c
typedef struct net_tcp {
    uint8_t  dst_ip[4];
    uint8_t  dst_mac[6];
    uint16_t local_port, remote_port;

    uint32_t snd_nxt, snd_una;   /* Send sequence space */
    uint32_t rcv_nxt;            /* Receive sequence space */
    uint16_t snd_wnd, rcv_wnd;   /* Flow control */

    enum {
        TCP_CLOSED, TCP_SYN_SENT, TCP_SYN_RCVD,
        TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
        TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK,
        TCP_TIME_WAIT
    } state;

    tcp_rxring_t rx;

    /* Out-of-order buffer (4 Slots) */
    struct { uint32_t seq; uint16_t len; uint16_t off; } ooo[4];
    int ooo_count;

    /* Retransmit */
    uint64_t rto_ms, last_send_ms;

    uint8_t got_fin, got_rst;
} net_tcp_t;
```

## Config (v2)

```c
#define NET_PKT_SIZE        1536
#define NET_TCP_RXBUF       65536   /* 64KB pro Socket */
#define NET_TCP_MAX         32      /* Max TCP-Verbindungen */
#define NET_UDP_MAX         16      /* Max UDP-Sockets */
#define NET_TCP_OOO_SLOTS   4       /* Out-of-Order Buffer */
#define NET_ARP_CACHE       16      /* ARP-Cache Eintraege */
#define NET_TCP_INIT_RTO_MS 1000
#define NET_TCP_MAX_RTO_MS  60000
/* Kein NET_QUEUE_SIZE — Per-Socket Ringbuffer */
/* Kein NET_TCP_TIMEOUT_MS — SO_RCVTIMEO pro Socket, default unendlich */
```

## Migrations-Plan

### Phase 0 — Quick-Fixes (npm unblockieren)

Minimal-Aenderungen an bestehendem Code. Kein Refactoring.

- NET_QUEUE_SIZE 16→128
- NET_TCP_RXBUF 4096→65536
- NET_TCP_TIMEOUT_MS 5000→30000
- TCP recv: timeout → -EAGAIN statt 0
- TCP recv: non-matching Pakete re-queuen statt droppen
- flock(73) Stub
- MAX_SOCKETS 16→64

### Phase A — tcp.c extrahieren

TCP-Code aus net.c in tcp.c verschieben.
Per-Socket Ringbuffer statt globale q_tcp.
TCP State-Machine (10 States).
Hash-Lookup fuer tcp_find().

### Phase B — udp.c extrahieren

UDP-Code aus net.c in udp.c.
Per-Socket Demux statt globale q_udp_sock.
Eliminiert Port-Mismatch Re-Queue.

### Phase C — dispatch.c + arp.c + ip.c

Packet-Dispatch aus net.c in dispatch.c.
ARP-Cache als eigenes Modul.
IP-Header-Helpers in ip.c.

### Phase D — dns.c + dhcp.c

DNS-Resolver und DHCP-Client als eigene Module.
net.c wird zu ~50 Zeilen (NIC-Registration + net_init).

### Phase E — Robustheit

- Out-of-Order Segment Buffering
- Slow-Start + Congestion Avoidance (AIMD)
- Non-Blocking TCP Connect (EINPROGRESS)
- SO_RCVTIMEO / SO_SNDTIMEO pro Socket
- TCP Keepalive
- RST auf unbekannte Segmente

## Test-Strategie (test/net/)

### Unit-Tests (in-Kernel, ktest-Framework)

Testen interne Funktionen ohne Netzwerk.

- `test_tcp_state.c` — State-Machine Transitions
- `test_tcp_rxring.c` — Ringbuffer push/pop/wrap
- `test_arp_cache.c` — ARP Cache Insert/Lookup/Evict
- `test_ip_checksum.c` — IP/TCP/UDP Checksums
- `test_tcp_ooo.c` — Out-of-Order Buffer

### Integrations-Tests (Userspace, QEMU mit Netzwerk)

Testen den vollstaendigen Pfad Syscall → Kernel → NIC → Internet.

- `test_net_dns.c` — DNS-Aufloesung gegen 10.0.2.3 (QEMU slirp)
- `test_net_tcp_connect.c` — TCP-Connect zu example.com:80
- `test_net_tcp_transfer.c` — HTTP GET, Response-Body pruefen
- `test_net_tls.c` — HTTPS via Node.js, Zertifikat-Validierung
- `test_net_udp_echo.c` — UDP Send/Recv Roundtrip
- `test_net_multi_conn.c` — 4 parallele TCP-Verbindungen
- `test_net_npm_registry.c` — HTTPS GET registry.npmjs.org, JSON parsen

### End-to-End-Tests (Boot-Test, gegen echte Server)

Laufen als Teil von boot-test.sh im QEMU-Disk-Modus.

- DNS lookup (example.com via /etc/hosts)
- HTTPS GET example.com (Status 200)
- HTTPS GET registry.npmjs.org (Status 200)
- npm --version
- npm install -g (wenn Phase 0+ fertig)
- claude update (Endziel)
