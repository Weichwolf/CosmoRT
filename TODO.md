# CosmoRT — Offene Punkte

Stand: 2026-03-24. 415 ktest PASS. SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 laufen (--version).
DHCP ok. Kernel-DNS + TCP + HTTP zu Internet-Servern ok.
claude update blockiert an E1000-RX in Multi-Process-Kontext.

---

## Netzwerk — Blocker: E1000 RX in Multi-Process

Kernel-Level (single-thread): DNS, TCP, HTTP zu example.com ok.
Node.js (multi-thread/process): E1000 empfängt keine Pakete.

Hypothese: net_poll() im Syscall-Kontext eines Fork-Childs kann
keine E1000-Pakete lesen. Funktioniert nur als init-Prozess.

### Naechste Schritte
- [ ] E1000 RX-Ring im Multi-Process-Kontext debuggen
- [ ] Alternative: virtio-net statt E1000 (einfacherer Treiber)
- [ ] UDP recvmsg/sendmmsg fuer c-ares DNS
- [ ] TCP connect fuer HTTPS (Node.js TLS)
- [ ] claude update erfolgreich

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
