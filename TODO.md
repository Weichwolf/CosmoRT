# CosmoRT — Offene Punkte

Stand: 2026-03-23. 373 ktest PASS. SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 laufen. Interaktive bash via Serial.

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

## Node.js — GEFIXT

- [x] BUG-SIG1: Red-Zone-Subtraktion (stack_rsp -= 128)
- [x] BUG-SIG2: RSP-Alignment (((sp - frame) & ~0xF) - 8)
- [x] BUG-SIG3: sig_blocked per-Thread (thread_t statt process_t)
- [x] TCGETS Stack-Smash: 60→36 Bytes (Kernel-termios)
- [x] mmap Hint ignoriert: vma_find_free_above fuer V8-Cage
- [x] AP-Core CR0.EM/MP: SSE auf Compute-Cores aktiviert
- [x] Kernel -mno-sse: Timer-ISR korrumpierte XMM-Register
- [x] VMA-Lock: spin_lock_irq fuer alle VMA-Operationen
- [x] FD_SERIAL TTY-Compat: sane termios-Defaults + FIONREAD
- [x] FIONBIO ioctl: O_NONBLOCK setzen/loeschen
- [x] /dev/tty: PTY_SLAVE statt Device
- [x] pselect6/select: Konvertierung zu poll
- [x] PTY poll readiness: EPOLLIN/EPOLLOUT
- [x] TIOCGPGRP: Caller-pgid wenn fg_pgid==0
- [x] PTY-Read Syscall-Restart statt -EAGAIN
- [x] Serial↔VT Bridge: Interaktive bash ueber serial stdio
- [x] Node.js laeuft ✓
- [x] Claude Code --version laeuft ✓

## Neue Findings aus Testsuite-Planung

### SEC-CRIT
- [ ] rt_sigreturn: RIP/RSP aus User-ucontext ohne Validierung (Kernel-Exec moeglich)

### CORR-CRIT — GEFIXT
- [x] FPU/SSE Save/Restore bei Context-Switch (fxsave/fxrstor in sched_preempt + thread_run)
- [x] FS_BASE in rt_sigreturn restauriert
- [x] do_fork() kopiert fs_base ins Child

## Netzwerk

- [ ] DHCP funktioniert nicht (E1000 IRQ Sharing mit virtio-blk)
- [ ] GPT-Image Boot (Kernel hat keinen Partitions-Support)
- [ ] AF_INET6 (IPv6)

## Audit 3+4 — Verbleibende Findings

### GEFIXT
- [x] rt_sigreturn RIP/RSP/RFLAGS Validierung (SEC-CRIT)
- [x] do_recvfrom/socket_write Bounce-Buffer (SEC-HIGH)
- [x] do_readv iovcnt EINVAL statt Clamping (CORR-MED)
- [x] do_kill pid=-1 alle Prozesse (CORR-MED)
- [x] CSI-Param Overflow-Guard (MED)
- [x] dmesg_ring spinlock (MED)
- [x] brk-Shrink: VMA vor unmap (MED)

### Audit 4 — In Arbeit (Agents)
- [ ] #1 mmap file-backed Lock-Release Race
- [ ] #2 mmap addr+length Ceiling-Check
- [ ] #3 order_for_pages signed shift UB
- [ ] #4 TLB-Shootdown Atomizitaet
- [ ] #5 vma_find_free gap_end-size Wrap
- [ ] #6 usock_write peer Use-after-free
- [ ] #7 unix_socket ring ohne Sync
- [ ] #8 Accept Socket Leak
- [ ] #9 kill_one nicht-atomarer State-Wechsel
- [ ] #11 deliver_signal stale RCX/R11
- [ ] #12 sched_preempt Signal-Path RCX
- [ ] #13 cosmofs_inode_read statischer Buffer
- [ ] #14 futex pi_boost ohne Sched-Lock
- [ ] #15 EPOLLET Extra-Event
- [ ] #17 rt_sigreturn FS_BASE Validierung

### Verbleibend
- [ ] SYS_COSMO_FW_LOAD: Output-Pointer
- [ ] SYS_COSMO_NIC_ATTACH: sizeof(kargs) statt hardcoded 22
- [x] PID/TID Wraparound: wrappen bei TABLE_MAX
- [ ] pipe_slab_ensure Race
- [x] timer_sleep_ms: Busywait <10ms ist Design (HW-Timing), kein Bug
- [ ] inotify_event Pool-Scan
- [x] TD14: Loopback 127.x.x.x → RX-Queue

## Interaktive Shell — Verbleibend

- [ ] Job Control (bash ohne +m Flag)
- [ ] Dynamic Linker: cat/coreutils crashen (RIP=0x0)
- [ ] Ctrl-C (SIGINT an Foreground-Prozessgruppe)
