# CosmoRT — Offene Punkte

Stand: 2026-03-24. 415 ktest PASS. SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 laufen. Interaktive bash via Serial.

---

## Interaktive Shell

- [ ] Ctrl-C (SIGINT an Foreground-Prozessgruppe via PTY)
- [ ] Dynamic Linker: cat/coreutils crashen (RIP=0x0, ld-cosmo.so)
- [ ] Job Control (bash ohne +m Flag — TIOCSPGRP, SIGTSTP/SIGCONT)

## Netzwerk

- [ ] DHCP funktioniert nicht (E1000 IRQ Sharing mit virtio-blk)
- [ ] GPT-Image Boot (Kernel hat keinen Partitions-Support)
- [ ] AF_INET6 (IPv6)

## Audit — Reste

- [ ] SYS_COSMO_FW_LOAD: Output-Pointer validieren
- [ ] SYS_COSMO_NIC_ATTACH: sizeof(kargs) statt hardcoded 22
- [ ] inotify_event: Scannt alle Pool-Entries bei jedem VFS-Event (PERF)

## Refactoring (TD)

- [ ] TD7: inline-asm Extraktion → arch_*() Interface, src/kernel/ asm-frei
- [ ] TD8: Cold-Path Strings aus Hot-Path extrahieren (irq_dispatch etc.)
- [ ] TD9: Bottom-Up Sortierung aller .c Dateien
