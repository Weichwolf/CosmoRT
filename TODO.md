# CosmoRT — Offene Punkte

Stand: 2026-03-24. 509 ktest PASS. SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 laufen (--version).
DHCP ok. Node.js HTTPS zu Internet-Servern ok (Boot-Test 11/12 PASS).

---

## Netzwerk

- [x] E1000 RX Multi-Process: net_udp_recv while→do-while Fix (non-blocking timeout=0)
- [x] Node.js HTTPS zu example.com (Boot-Test T4)
- [ ] c-ares UDP DNS-Resolution (sendmmsg+recvfrom non-blocking, brk-Kollision?)
- [ ] claude update erfolgreich

## Interaktive Shell

- [ ] Ctrl-C (SIGINT an Foreground-Prozessgruppe via PTY)
- [ ] Dynamic Linker: cat/coreutils crashen (RIP=0x0, ld-cosmo.so)
- [ ] Job Control (bash ohne +m Flag — TIOCSPGRP, SIGTSTP/SIGCONT)

## Boot

- [ ] GPT-Image Boot (Kernel hat keinen Partitions-Support)

## Audit — erledigt

- [x] SYS_COSMO_FW_LOAD: Input-Pointer (name) validiert
- [x] SYS_COSMO_NIC_ATTACH: sizeof(kargs) (war bereits korrekt)
- [x] inotify_event: Lockless pre-check auf watch_count==0

## Refactoring (TD) — erledigt

- [x] TD7: arch_x86.h komplett — alle Kernel-.c ohne inline-asm
- [x] TD8: Cold-Path Strings extrahiert
- [x] TD9: Bottom-Up Sortierung komplett — alle Dateien geprueft, net.c mDNS-Block reorderiert
