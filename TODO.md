# CosmoRT — Offene Punkte

Stand: 2026-03-24. 415+ ktest PASS. SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 laufen (--version).
DHCP ok. Kernel-DNS + TCP + HTTP zu Internet-Servern ok.

---

## Netzwerk — Blocker: E1000 RX in Multi-Process

Kernel-Level (single-thread): DNS, TCP, HTTP zu example.com ok.
Node.js (multi-thread/process): E1000 empfaengt keine Pakete nach DHCP.
claude update: DNS-Resolution scheitert (c-ares sendmmsg funktioniert,
aber UDP-Reply kommt nicht zurueck).

### Naechste Schritte
- [ ] E1000 RX-Ring im Multi-Process-Kontext debuggen
- [ ] Alternative: virtio-net statt E1000
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

## Refactoring (TD) — Teilweise erledigt

- [x] TD7: arch_x86.h erstellt, main.c + sys_ipc.c migriert (weitere Dateien offen)
- [x] TD8: Cold-Path Strings extrahiert (sys_mem.c, irq.c)
- [x] TD9: Bottom-Up Sortierung (sys_mem.c, sched.c, process.c, irq.c, sys_proc.c, sys_signal.c)
- [ ] TD7: Verbleibende Dateien auf arch_*() migrieren
- [ ] TD9: Verbleibende Dateien sortieren (sys_file.c, vfs.c, etc.)
