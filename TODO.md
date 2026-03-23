# CosmoRT — Offene Punkte

Stand: 2026-03-23. 366 ktest PASS. SMP 2. RT+Compute Core-Modell.

---

## TD — Offene Arbeitspakete

- [ ] TD8: Cold-Path Strings aus Hot-Path extrahieren (irq_dispatch etc.)
- [ ] TD9: Bottom-Up Sortierung aller .c Dateien
- [ ] TD10: Syscall-Fuzzer (zufaellige Args, Kernel muss ueberleben)
- [x] TD12: X-Macro Syscall-Tabelle ✓
- [x] TD13: copy_from_user/copy_to_user Pattern ✓
- [ ] TD14: Loopback (lo) — 127.0.0.1 direkt in RX-Queue

## TD7 — Unvollstaendig

- [ ] inline-asm Extraktion: arch_*() Interface fuer alle __asm__ in src/kernel/
- [ ] src/kernel/ soll kein inline-asm mehr haben (nur includes von arch-Header)
- [ ] GNU as (.S) statt NASM (.asm) fuer Konsistenz ueber Architekturen

## Node.js — Stack-Smash (3 Bugs in Signal-Delivery)

### BUG-SIG1: Fehlende Red-Zone-Subtraktion (SEC-CRIT)
- Datei: src/kernel/syscall/sys_signal.c, deliver_signal(), Zeile ~148
- Code: `new_rsp = (stack_rsp - frame_size) & ~0xFULL`
- Problem: x86_64 ABI Red Zone (128 Bytes unter RSP) wird nicht respektiert.
  Signal-Frame ueberschreibt Red-Zone-Daten von Leaf-Funktionen.
- Fix: `stack_rsp -= 128` vor Frame-Berechnung
- Linux: arch/x86/kernel/signal.c get_sigframe() macht `sp -= 128`

### BUG-SIG2: Falsches RSP-Alignment im Signal-Frame (CORR-HIGH)
- Datei: gleiche Stelle
- Code: `& ~0xFULL` ergibt RSP ≡ 0 (mod 16)
- Problem: ABI erwartet RSP ≡ 8 (mod 16) bei Funktions-Entry
- Fix: `((sp + 8) & ~0xFULL) - 8` statt `sp & ~0xFULL`
- Linux: align_sigframe() macht round_down(sp, 16) - 8

### BUG-SIG3: sig_blocked ist process-level statt per-Thread (CORR-HIGH)
- Datei: include/internal/process.h
- Problem: sig_blocked in process_t geteilt zwischen allen Threads.
  Linux hat blocked per task_struct (per Thread).
  Bei Node.js (clone CLONE_VM): Race auf sig_blocked, falsche Masken.
- Fix: sig_blocked von process_t nach thread_t verschieben

### Naechste Schritte
- [ ] Tests schreiben die BUG-SIG1/SIG2/SIG3 reproduzieren
- [ ] Alle drei Bugs fixen
- [ ] Node.js erneut testen
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
