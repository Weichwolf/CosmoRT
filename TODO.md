# CosmoRT — Offene Punkte

Stand: 2026-03-30. ktest 1254/0. musl libc-test 454/18 (96.2%). LTP laeuft.

---

## Busy-Wait Elimination (blockiert alles)

Busy-Wait-Polling bricht Timer, Signale, sleep/alarm/timeout.

### Erledigt

- [x] P0-A: Timer — LAPIC one-shot + arch_halt, 15 ktest Timer-Tests
- [x] P0-B: Netzwerk-Boot — event_wait in ARP/DNS, lapic_delay_ms in Boot-DHCP

### SMP + TLB

- [ ] SMP AP boot: LAPIC timer statt pure spin-wait (smp.c:176)
- [ ] TLB shootdown: LAPIC timer + Timeout statt 10M-Iteration spin-wait (irq.c:733)
- [ ] Kexec AP shutdown: LAPIC timer statt 2M-Iteration spin-wait (kexec.c:137)

### IPC

- [ ] IPC receive: event_wait auf Endpoint statt 100-Iteration spin (ipc.c:133)
- [ ] IPC wait_any: event_wait auf ANY Endpoint statt Doppel-Scan (ipc.c:195)

### Timerfd + Poll

- [ ] Timerfd expiry: Interrupt-Callback statt tight poll-loop (sys_ipc.c:461)
- [ ] Socket/epoll deadline checks: TSC-basiert statt timer_ms() vor event_wait

---

## Natives RT (Kern-Architektur)

- [ ] NOHZ_FULL: Timer-Ticks auf isolierten Cores eliminieren (ohne: 1ms Jitter)
- [ ] RCU (Read-Copy-Update): lock-free Read-Side fuer Kernel-Datenstrukturen
- [ ] Spinlocks -> PI-Mutexes (sleeping locks) im normalen Kernel-Pfad
- [ ] Adaptive Spinning: kurzer Spin vor Mutex-Sleep wenn Owner laeuft
- [ ] Threaded IRQs (Hard-IRQ nur ACK+Wake, Verarbeitung im Thread)
- [ ] Preemption-Modell: Full + Lazy Preemption spezifizieren und implementieren
- [ ] Bounded WCET auf allen Pfaden (keine unbounded Loops)
- [ ] CPU-Frequency-Invarianz: WCET kompensiert Turbo-Boost/P-States
- [ ] isolcpus fuer dedizierte Latenz-Cores
- [ ] Latenz-Messung + ktest Assertions (<10us WCET Ziel)

---

## CPU-Features (Korrektheit + Performance)

### XSAVE / AVX2

FXSAVE sichert nur SSE. AVX-State wird bei Context-Switch korrumpiert.

- [ ] XSAVE/XRSTOR statt FXSAVE/FXRSTOR in Context Switch, fork, Signal
- [ ] XSAVE Area Groesse per CPUID (CPUID 0xD) statt feste 512 Bytes
- [ ] Signal-Frame: XSAVE Area statt FXSAVE in fpstate
- [ ] sigreturn: XSAVE Area validieren
- [ ] CR4.OSXSAVE + XCR0 Setup bei Boot

### FSGSBASE

Ohne FSGSBASE: jeder TLS-Zugriff braucht WRMSR (privilegiert, langsam).

- [ ] CR4.FSGSBASE=1 bei Boot
- [ ] WRFSBASE/RDFSBASE fuer Userspace TLS (arch_prctl SET_FS Fastpath)

### PCID + INVPCID

Ohne PCID: jeder Context-Switch flusht TLB komplett.

- [ ] CR4.PCIDE=1, PCID pro Prozess zuweisen
- [ ] INVPCID fuer gezielte TLB-Invalidierung
- [ ] TLB-Shootdown mit INVPCID statt Full-Flush

### x2APIC

MSR-basiert statt MMIO. Schnellere EOI, IPI, Timer-Writes.

- [ ] x2APIC Mode aktivieren (IA32_APIC_BASE MSR Bit 10)
- [ ] Alle LAPIC-Zugriffe auf MSR umstellen

### TSC-Deadline Mode

Sub-Mikrosekunden-Timer-Praezision. Direkt fuer <10us RT-Ziel.

- [ ] IA32_TSC_DEADLINE MSR statt LAPIC-Divider fuer One-Shot Timer
- [ ] lapic_delay_ms / Timer-Callbacks auf TSC-Deadline umstellen

---

## ext4 Migration (Korrektheit)

ext2 hat kein Journal, keine Extents, 32-Bit Timestamps (Y2038).

- [ ] ext4 Extent-Tree lesen (statt Direct/Indirect Blocks)
- [ ] ext4 Journal (JBD2): Replay bei Mount, Commit bei Write
- [ ] ext4 64-Bit Timestamps (inode extra fields)
- [ ] ext4 Metadata Checksums
- [ ] mkfs.ext4 in Build-Pipeline (mkimage.sh, Makefile)
- [ ] Header/Source umbenennen: ext2.h/ext2.c -> ext4.h/ext4.c

---

## Signal-Delivery

- [x] SIGSTOP/SIGCONT, SIGALRM, SIGTERM/SIGKILL, Remote Kill, SIGCHLD, nanosleep restart
- [ ] nanosleep re-block: nach Signal-Delivery verbleibende Zeit weiterschlafen
- [ ] SIGALRM an blockierte Prozesse: event_wait muss ALLE Signale pruefen

---

## Prozesse/Threads

- [ ] CLONE_THREAD korrekt: echte Threads (gleiche PID, verschiedene TIDs)
- [ ] clone3 (struct clone_args)
- [ ] sendfile: zero-copy file transfer
- [ ] getrusage korrekt: CPU-Zeit pro Prozess/Thread

---

## Shared Memory + Cross-Process

- [x] MAP_SHARED fork, Shared Futex
- [ ] MAP_SHARED anonymous coherency
- [ ] MAP_SHARED file-backed: Page Cache Coherency
- [ ] /dev/shm tmpfs: mmap coherency

---

## Alpine IPC Blocker

- [ ] inotify echte Events (IN_CREATE, IN_MODIFY, IN_DELETE)
- [ ] SCM_RIGHTS: fd-Passing ueber Unix Socket
- [ ] Abstract Unix Sockets (@ Namespace)
- [ ] flock echtes Advisory Locking
- [ ] SysV IPC: shmget/shmat/shmctl, semget/semop/semctl, msgget/msgsnd/msgrcv/msgctl

---

## musl libc-test Remaining (18 FAIL)

- [ ] fma, fmal, powf, remquol: FPU Precision
- [ ] pthread_robust (2): robust mutex Timeout
- [ ] sem_open (2): shared mmap coherency
- [ ] socket (2): loopback TCP accept
- [ ] utime (2): Y2038 (ext4 Migration loest das)
- [ ] pthread_atfork (2): errno clobber bei fork in multithreaded
- [ ] rlimit-open-files (2): RLIMIT_NOFILE enforcement
- [ ] malloc-brk-fail (1): brk OOM recovery
- [ ] tls_get_new-dtv (1): dynamic TLS segment allocation
- [ ] raise-race (2): fork in Signal-Handler + RT signals

---

## LTP Test-Compliance

### Bestanden (10 PASS)

- [x] abort01, accept01, accept03, access02, access03, alarm02/03/05/06

### Fehlend

- [ ] accept02, accept4_01: Loopback TCP accept
- [ ] access01, access04: Permission-Bits (chmod/fchmod enforcement)
- [ ] acct01: Process Accounting
- [ ] adjtimex01-03: adjtimex Modes + Validation
- [ ] alarm07: fork + sleep(3) + alarm — nanosleep re-block Bug

---

## Netzwerk

- [x] Loopback TCP: ARP-Bypass
- [ ] Loopback TCP accept: SYN aus q_tcp verarbeiten
- [ ] IPv6 (RFC 8200) + NDP + SLAAC

---

## Security

- [ ] CET: Shadow Stack (SHSTK) + Indirect Branch Tracking (IBT)
- [ ] UMIP: User-Mode Instruction Prevention (CR4.UMIP=1)
- [ ] eIBRS/AutoIBRS (CPUID-Check, mandatory)
- [ ] IBPB bei Kontextwechsel
- [ ] KPTI + PCID (CPUID-conditional, nur Meltdown-anfaellige CPUs)
- [ ] SSBD
- [ ] IOMMU (VT-d/AMD-Vi): DMA-Remapping fuer Userspace-Treiber

---

## Hardware-Modernisierung

- [ ] PCI: ECAM (MMIO Config) statt Legacy Port I/O
- [ ] PCI: MSI-X Interrupt-Routing
- [ ] Serial: IRQ-driven TX/RX statt Polling

---

## Observability

- [ ] /proc/self/fd/ Directory Listing
- [ ] /proc/pid/stat komplett
- [x] /proc/PID/oom_score_adj
- [ ] /proc/sys/kernel/threads-max
- [ ] perf_event_open (Hardware Performance Counters)

---

## Terminal / VT

- [ ] Alternate Screen (?1049h/l)

---

## Device Nodes (SDL3/UI)

- [ ] /dev/fb0, /dev/input/event0, /dev/snd/, /dev/dri/
- [ ] VT-Slots: 12 Fullscreen-Surfaces (F1-F12), Device-Routing pro Slot
- [ ] Audio-Mixer: 12-Spur, pro Slot

---

## Moderne Syscalls (DESIGN.md §7)

- [ ] io_uring (io_uring_setup/enter/register)
- [ ] memfd_create
- [ ] copy_file_range
- [ ] close_range
- [ ] pidfd_open
- [ ] pidfd_send_signal

## Fehlende Syscalls

- [ ] ARCH_SET_GS/ARCH_GET_GS
- [ ] sendfile, splice/tee/vmsplice
- [ ] Hard Links in ext4
- [ ] renameat2 RENAME_EXCHANGE
- [ ] MSG_ZEROCOPY (zero-copy send)
- [ ] TIOCSPGRP ioctl

### Erledigt

- [x] prlimit64 set, Robust Mutex, adjtimex, WCOREDUMP, O_PATH
- [x] futimens, utimensat UTIME_NOW/UTIME_OMIT, OOM Guard

---

## Crypto

- [ ] AES (FIPS 197)
- [ ] AES-NI Hardware-Beschleunigung (CPUID-Check)
- [ ] SHA-NI Hardware-Beschleunigung (CPUID-Check)

---

## Skalierung (niedrige Prio)

PTY Pool, epoll_ctl Hash, Unix Socket Slab, etc.
