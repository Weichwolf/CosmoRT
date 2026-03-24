# CosmoRT — Offene Punkte

Stand: 2026-03-24. 415 ktest PASS. SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 laufen. Interaktive bash via Serial.
DHCP ok. UDP-Sockets implementiert. TCP SYN-ACK geht verloren (Polling-Bug).

---

## Netzwerk — Architektur-Umbau (Blocker fuer claude update)

Aktueller Zustand: Polling-basierter Net-Stack im Kernel. net_poll() liest
ein Paket pro Aufruf, TCP/UDP blockieren in Busy-Loops. Funktioniert fuer
DHCP/ARP, aber TCP-Verbindungen scheitern (SYN-ACK geht verloren).

### Phase 1: E1000-Treiber nach Ring 3

- [ ] e1000d Userspace-Treiber aktivieren (Binary existiert: /bin/e1000d)
- [ ] MMIO-Zugriff via cosmo_mmio_map (cosmo_rt.h API)
- [ ] DMA-Allokation via cosmo_dma_alloc
- [ ] IRQ-Notification via cosmo_irq_register → eventfd/pipe Wakeup
- [ ] net_port IPC: Treiber → Kernel Paket-Uebergabe (TX/RX Ringbuffer)
- [ ] Kernel-seitiger NIC-Code entfernen (e1000.c, virtio_net.c → nur Stubs)

### Phase 2: Event-basierter Net-Stack

- [ ] Socket RX-Queue pro Socket (statt globale q_tcp/q_udp)
- [ ] Paket-Eingang: NIC-IRQ → e1000d → net_port → Kernel Dispatcher
- [ ] Dispatcher: demultiplex nach Proto/Port → Socket-Queue
- [ ] Socket-Queue weckt epoll/poll-Waiter (EPOLLIN)
- [ ] TCP-Connect/Send/Recv non-blocking mit epoll-Integration
- [ ] UDP sendto/recvfrom event-basiert

### Phase 3: Vollstaendige Konnektivitaet

- [ ] DNS-Resolution via UDP (Node.js c-ares)
- [ ] TCP-Connect zu externen Hosts (HTTPS fuer claude update)
- [ ] TLS funktioniert (Node.js OpenSSL nutzt TCP-Sockets)
- [ ] claude update erfolgreich
- [ ] AF_INET6 (IPv6)

## Interaktive Shell

- [ ] Ctrl-C (SIGINT an Foreground-Prozessgruppe via PTY)
- [ ] Dynamic Linker: cat/coreutils crashen (RIP=0x0, ld-cosmo.so)
- [ ] Job Control (bash ohne +m Flag — TIOCSPGRP, SIGTSTP/SIGCONT)

## Boot

- [ ] GPT-Image Boot (Kernel hat keinen Partitions-Support)

## Audit — Reste

- [ ] SYS_COSMO_FW_LOAD: Output-Pointer validieren
- [ ] SYS_COSMO_NIC_ATTACH: sizeof(kargs) statt hardcoded 22
- [ ] inotify_event: Scannt alle Pool-Entries bei jedem VFS-Event (PERF)

## Refactoring (TD)

- [ ] TD7: inline-asm Extraktion → arch_*() Interface, src/kernel/ asm-frei
- [ ] TD8: Cold-Path Strings aus Hot-Path extrahieren (irq_dispatch etc.)
- [ ] TD9: Bottom-Up Sortierung aller .c Dateien
