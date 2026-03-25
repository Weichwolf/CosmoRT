# CosmoRT — Offene Punkte

Stand: 2026-03-25. 509 ktest PASS, 0 FAIL.
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

- [x] UDP-Code aus net.c → udp.c
- [x] Per-Socket Demux statt globale q_udp_sock
- [x] test/unit/net/test_net_udp_echo.c

## Net Phase C — dispatch.c + arp.c + ip.c

- [x] Packet-Dispatch aus net.c → dispatch.c
- [x] ARP-Cache Modul → arp.c
- [x] IP-Header-Helpers → ip.c
- [x] test/net/test_arp_cache.c
- [x] test/net/test_ip_checksum.c

## Net Phase D — Userspace-Protokolle aus Kernel entfernen

~320 Zeilen Anwendungsprotokolle und Debug-Code gehoeren nicht in den Kernel.

### D1: Kernel-Module extrahieren (bleiben im Kernel, eigene Dateien)

- [x] DNS-Resolver net.c → src/kernel/net/dns.c (Kernel braucht DNS fuer Boot)
- [x] DHCP-Client net.c → src/kernel/net/dhcp.c (Kernel braucht DHCP fuer Boot)

### D2: In Userspace verschieben (raus aus dem Kernel)

- [x] net_http_get() → entfernt (Userspace: curl/wget/Node.js)
- [x] net_ping() → entfernt (Userspace: /bin/ping ueber Raw-Socket)
- [x] mDNS (net_set_hostname, mdns_respond, mdns_handle) → entfernt (Userspace-Daemon)
- [x] procfs_nettest() → entfernt (Userspace-Integrationstests)

### D3: Debug-Code aufräumen

- [x] serial_puts Debug-Ausgaben in tcp.c entfernt (8 Stellen)
- [x] IP-Adress-Formatierung in tcp.c entfernt

## RT/Compute Schnittstelle — include/kernel/core/rt.h

Problem: Keine saubere Abstraktion zwischen RT-Core und Compute-Cores.
Kommunikation laeuft ueber globale Queues mit Spinlocks.
RT-Core darf nie blockieren, Compute-Cores duerfen nie IRQ-State anfassen.

### RT/Compute-A: Grundprimitives

- [x] arch.h: arch_store_release, arch_load_acquire, arch_wmb, arch_rmb
- [x] arch.h: arch_dma_sync_for_device, arch_dma_sync_for_cpu (x86: No-Op, ARM64: Cache-Ops)
- [x] rt_channel_t: SPSC Lock-free Ringbuffer (atomic head/tail via arch_store_release/load_acquire)
- [x] rt_channel_push(ch, msg, len) — Producer-Seite, non-blocking
- [x] rt_channel_pop(ch, buf, len) — Consumer-Seite, non-blocking
- [x] rt_core_id(int index) — welcher physische Core ist RT-Core N
- [x] rt_is_current_rt() — true auf RT-Core(s), false auf Compute
- [x] test: rt_channel push/pop Roundtrip
- [x] test: rt_channel wrap-around bei vollem Buffer
- [x] test: rt_is_current_rt() korrekt auf beiden Core-Typen

### RT/Compute-B: IPI + Wake

- [ ] rt_wake(int core_id) — IPI an Ziel-Core senden
- [ ] sched_wake(thread_t *t) — markiert Thread runnable, sendet IPI falls anderer Core
- [ ] IPI-Handler auf Compute-Core: Scheduler-Reschedule ausloesen
- [ ] test: rt_wake IPI kommt an
- [ ] test: sched_wake weckt schlafenden Thread auf anderem Core

### RT/Compute-C: TX-Ring (Compute→RT fuer Netzwerk-TX)

- [ ] tx_ring_t pro NIC: SPSC, Compute=Producer, RT=Consumer
- [ ] send() Syscall: TCP-Paket bauen → tx_ring_push
- [ ] net_poll() auf RT-Core: tx_ring_drain() → nic->send()
- [ ] Doorbell-Flag (atomic): Compute setzt nach Push, RT prueft im IRQ-Return-Path
- [ ] test: TX-Ring Durchsatz (Pakete/s)

### RT/Compute-D: Timer-Wheel (RT-Core owned)

- [ ] timer_wheel_t auf RT-Core: 1ms Granularitaet, 256 Slots
- [ ] rt_timer_request(sock, action, timeout_ms) — Compute postet in Timer-Request-Ring
- [ ] RT-Core: Timer-Wheel tick → faellige Timer feuern → Aktion direkt ausfuehren
- [ ] Timer-Actions: TCP Retransmit, Keepalive, DHCP Renewal (alle Netzwerk-Sends)
- [ ] test: Timer feuert nach Deadline
- [ ] test: Timer-Cancel vor Deadline

### RT/Compute-E: Prioritaeten auf RT-Core

- [ ] rt_poll() prueft in statischer Prio-Reihenfolge:
      P0 Audio (<5ms) → P1 Input/HID (<1ms) → P2 Net-RX (max 64 pkt) →
      P3 Net-TX (max 64 pkt) → P4 VSync/DMA → P5 Timer-Wheel
- [ ] Bounded Work: Netzwerk max N Pakete pro Durchlauf, dann Prio-Check
- [ ] Handler returniert "more_work" Flag → naechste Runde nach Prio-Pruefung
- [ ] test: Audio-Callback unterbricht Netzwerk-Burst

### RT/Compute-F: Skalierung (Abstraktion, nicht Implementierung)

- [ ] RT_CORE_COUNT=1 (spaeter: 2 fuer IRQ-Split)
- [ ] rt.h abstrahiert ueber Core-Count, hardcoded Core 0 nirgends
- [ ] Escape-Hatches dokumentiert: Multi-RT, NIC-Offload, Protocol-auf-Compute

## Net Phase E0 — Polling eliminieren (Sleep/Wake statt Busy-Wait)

Problem: net_tcp_recv pollt q_tcp in Busy-Wait-Loop mit timer_ms() Deadline.
Verschwendet CPU, blockiert Compute-Core, 30s Timeout bei leerem accept.

Architektur: RT-Core (IRQ) → Lock-free Ringbuffer → IPI → Compute-Core (Wake).
RT-Core darf nie blockieren, nie Spinlock halten der Compute-Core gehoert.

- [ ] Lock-free Ringbuffer fuer rxring (atomic head/tail, kein Spinlock)
- [ ] IPI-basiertes Wake: RT-Core schreibt Ringbuffer, sendet IPI an Ziel-Compute-Core
- [ ] sched_wake(thread_t *t) — markiert Thread runnable, sendet IPI falls anderer Core
- [ ] sock_waitq: Thread-ID + Timeout pro Socket (kein wait_queue Spinlock auf RT-Core)
- [ ] tcp_input() auf RT-Core: rxring_push (lock-free) → sched_wake via IPI
- [ ] net_tcp_recv auf Compute-Core: Ringbuffer leer → Thread suspendieren, Timeout setzen
- [ ] net_tcp_accept: keine pending SYN → Thread suspendieren bis SYN-Wakeup
- [ ] net_tcp_connect: nach SYN → Thread suspendieren bis SYN-ACK-Wakeup
- [ ] udp recv analog: Queue leer → Thread suspendieren
- [ ] q_tcp eliminieren — alle Pakete direkt in Per-Socket Ringbuffer via tcp_input
- [ ] net_idle() Aufrufe in TCP/UDP entfernen
- [ ] test: recv blockt bis IRQ Daten liefert (kein Polling)
- [ ] test: accept blockt bis SYN kommt (kein Timeout-Polling)

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

## MM — Copy-on-Write fork()

Problem: fork() deep-copied jede Seite (process.c:544-548). Node.js Worker-fork
bei 500MB Heap = 500MB kopiert, auch wenn Child nur 2MB aendert.

- [ ] COW-Bit im PTE: fork markiert alle User-Pages read-only in beiden Prozessen
- [ ] Page-Fault-Handler: Schreibzugriff auf COW-Page → neue Page allozieren, kopieren, PTE writable
- [ ] Refcount pro physische Page (page_alloc.h): fork incrementiert, munmap/exit decrementiert
- [ ] fork() wird O(Page-Tables) statt O(Speicher): nur PTEs kopieren, nicht Daten
- [ ] COW-sichere Kernel-Zugriffe: copy_from_user darf COW-Pages nicht triggern
- [ ] test: fork + child write → eigene Page, parent unveraendert
- [ ] test: fork ohne write → keine neuen Pages alloziert
- [ ] test: exit nach fork → refcount korrekt decrementiert

## MM — MADV_FREE (V8 Heap Management)

Problem: V8 gibt Heap-Seiten mit madvise(MADV_FREE) zurueck. Kernel darf sie bei
Speicherdruck recyclen, Prozess behaelt den VA-Range. Aktuell nur MADV_DONTNEED
(zerstoert sofort), V8 braucht lazy reclaim.

- [ ] VMA-Flag VM_LAZYFREE: Seiten als reclaimable markieren statt sofort freigeben
- [ ] Page-Reclaim: unter Speicherdruck LAZYFREE-Seiten zuerst freigeben
- [ ] Dirty-Check: LAZYFREE-Seite die erneut beschrieben wird verliert LAZYFREE-Status
- [ ] MADV_FREE in do_madvise (sys_mem.c) implementieren
- [ ] test: madvise(MADV_FREE) + erneuter Zugriff → Seite noch da (kein Druck)
- [ ] test: madvise(MADV_FREE) + Speicherdruck → Seite weg, erneuter Zugriff → Zero-Page

## MM — Transparent Huge Pages (2MB)

Problem: 500MB Node.js Heap = 128.000 4KB-PTEs = TLB-Thrashing.
2MB Huge Pages reduzieren auf ~250 PTEs.

- [ ] 2MB-Page-Allocator (Buddy oder freelist fuer order-9 Pages)
- [ ] PTE-Promotion: 512 zusammenhaengende 4KB-Pages mit gleichen Flags → 1 PMD-Entry (2MB)
- [ ] Automatische Promotion bei mmap(MAP_ANONYMOUS) >= 2MB-aligned
- [ ] Page-Fault auf 2MB-Page: direkt 2MB allozieren wenn alignment+size passen
- [ ] Fallback auf 4KB wenn 2MB nicht verfuegbar (keine Fragmentierung)
- [ ] COW-Interaktion: COW-Fault auf Huge Page → erst zu 4KB splitten, dann COW
- [ ] test: mmap 4MB aligned → 2 Huge Pages in PMD
- [ ] test: Huge Page + fork → COW split zu 4KB

## Offen (nicht Netzwerk)

- [ ] c-ares UDP DNS (c-ares ETIMEOUT trotz korrekter Pakete)
- [ ] Job Control (TIOCSPGRP, SIGTSTP/SIGCONT)
- [ ] Dynamic Linker (ld-cosmo.so — CosmoPX)
- [ ] GPT-Image Boot (Partitions-Support)
