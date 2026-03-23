# CosmoRT — Offene Punkte

Stand: 2026-03-23. 366 ktest PASS. SMP 2. RT+Compute Core-Modell.

---

## TD — Offene Arbeitspakete

- [ ] TD8: Cold-Path Strings aus Hot-Path extrahieren (irq_dispatch etc.)
- [ ] TD9: Bottom-Up Sortierung aller .c Dateien
- [ ] TD10: Syscall-Fuzzer (zufaellige Args, Kernel muss ueberleben)
- [ ] TD12: X-Macro Syscall-Tabelle (laeuft)
- [ ] TD13: copy_from_user/copy_to_user Pattern
- [ ] TD14: Loopback (lo) — 127.0.0.1 direkt in RX-Queue

## TD7 — Unvollstaendig

- [ ] inline-asm Extraktion: arch_*() Interface fuer alle __asm__ in src/kernel/
- [ ] src/kernel/ soll kein inline-asm mehr haben (nur includes von arch-Header)
- [ ] GNU as (.S) statt NASM (.asm) fuer Konsistenz ueber Architekturen

## Node.js

- [ ] Stack-Smash in Node.js (Buffer-Overflow in C++-Code, nicht Kernel-Bug)
- [ ] Node.js Rebuild mit -fno-stack-protector (laeuft in CosmoPX)
- [ ] Wenn Node.js laeuft: Claude Code testen

## Netzwerk

- [ ] DHCP funktioniert nicht (E1000 IRQ Sharing mit virtio-blk)
- [ ] GPT-Image Boot (Kernel hat keinen Partitions-Support)
- [ ] AF_INET6 (IPv6)

## Audit 3 — Verbleibende Findings

### SEC-HIGH (5 — teilweise gefixt durch Batch-Arbeit)
- [ ] do_recvfrom: TCP-Daten direkt in User-Buffer ohne Bounce
- [ ] socket_write: kein Bounce-Buffer

### SEC-MED (4)
- [ ] SYS_COSMO_FW_LOAD: Output-Pointer wenn implementiert
- [ ] SYS_COSMO_NIC_ATTACH: sizeof(kargs) statt hardcoded 22
- [ ] PID/TID Wraparound: next_pid/next_tid ohne Wrap-Check
- [ ] pipe_slab_ensure: Race bei konkurrenter Initialisierung

### CORR-MED (6 — teilweise gefixt)
- [ ] timer_sleep_ms: Busywait < 10ms blockiert Core
- [ ] do_readv: iovcnt Clamping statt EINVAL
- [ ] do_kill pid=-1: nur self statt alle

### PERF-MED (3)
- [ ] inotify_event: Scannt alle Pool-Entries bei jedem VFS-Event
