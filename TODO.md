# CosmoRT — Offene Punkte

Stand: 2026-03-30. ktest 1420/0. musl libc-test 454/18 (96.2%). LTP laeuft.

---

## Busy-Wait Elimination (blockiert alles)

Busy-Wait-Polling bricht Timer, Signale, sleep/alarm/timeout.

### Erledigt

- [x] P0-A: Timer — LAPIC one-shot + arch_halt, 15 ktest Timer-Tests
- [x] P0-B: Netzwerk-Boot — event_wait in ARP/DNS, lapic_delay_ms in Boot-DHCP

### SMP + TLB

- [x] SMP AP boot: arch_halt statt arch_pause in wait-loop (smp.c)
- [x] TLB shootdown: timer_ms deadline statt 10M-Iteration spin-wait (irq.c)
- [x] Kexec AP shutdown: lapic_delay_ms statt 2M-Iteration spin-wait (kexec.c)

### IPC

- [x] IPC receive: event_wait auf Endpoint statt 100-Iteration spin (ipc.c:133)
- [x] IPC wait_any: event_wait auf ANY Endpoint statt Doppel-Scan (ipc.c:195)

### Timerfd + Poll

- [x] Timerfd expiry: TSC-basiert statt timer_ms() poll-loop (timerfd.c, sys_ipc.c)
- [x] Socket/epoll deadline checks: TSC-basiert statt timer_ms() (epoll.c, socket.c)

---

## Natives RT (Kern-Architektur)

### RT-1: Preemption-Modell (Architektur, manuell)

- [x] Full + Lazy Preemption spezifizieren (wann preempted, wann deferred)
- [x] Preemption-Points im Scheduler dokumentieren
- [x] preempt_count + need_resched in thread_t
- [x] preempt_disable/preempt_enable/cond_resched API (core/preempt.h)
- [x] sched_preempt: preempt_count guard + RT immediate vs lazy preemption
- [x] DESIGN.md §13 aktualisiert
- [x] ktest: test_preempt.c (preempt infrastructure tests)

### RT-2: PI-Mutex (Agent)

- [ ] rt_mutex Datenstruktur + API (lock, unlock, trylock)
- [ ] Priority-Inheritance Chain (Boosting + Deboosting)
- [ ] ktest: PI-Inversion-Tests (High prio wartet auf Low prio)

### RT-3: Spinlock-Konvertierung (manuell, pro Subsystem)

- [ ] Audit: welche Spinlocks koennen schlafen, welche muessen spinnen (IRQ-Kontext)
- [ ] Subsystem-weise konvertieren (VFS, IPC, Netzwerk, Prozesse)
- [ ] Adaptive Spinning: kurzer Spin vor Mutex-Sleep wenn Owner laeuft

### RT-4: Threaded IRQs (manuell)

- [ ] Hard-IRQ Handler: nur ACK + Wake eines IRQ-Threads
- [ ] IRQ-Thread pro Vector (schedulebar, preemptibel)
- [ ] Abhaengig von RT-2 (IRQ-Threads brauchen PI-Mutex)

### RT-5: RCU (Agent, parallel zu RT-2..4)

- [ ] RCU Datenstruktur (grace period, callback queue)
- [ ] rcu_read_lock/rcu_read_unlock (preempt_disable/enable)
- [ ] synchronize_rcu / call_rcu
- [ ] ktest: concurrent read/write Tests

### RT-6: NOHZ_FULL (abhaengig von RT-4)

- [ ] Timer-Ticks auf isolierten Cores eliminieren
- [ ] Abhaengig von Threaded IRQs (kein periodischer Timer-IRQ noetig)

### RT-7: Unabhaengige Tasks (Agent)

- [ ] isolcpus: CPU-Isolation fuer Latenz-Cores
- [ ] CPU-Frequency-Invarianz: WCET kompensiert Turbo-Boost/P-States

### RT-8: Validierung (letzter Schritt)

- [ ] Bounded WCET Audit auf allen Pfaden
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

- [ ] /dev/fb0, /dev/input/event0, /dev/dri/
- [ ] VT-Slots: 12 Fullscreen-Surfaces (F1-F12), Device-Routing pro Slot

---

## Audio-Mixer 24-Kanal (DAW-Kern)

- [ ] 24-Kanal Stereo Mixer-Pool (dynamische Zuweisung an Slots)
- [ ] Kanal-zu-Kanal Routing (Audio-Graph)
- [ ] Graph-Ordering: topologischer Sort, sequentiell pro Audio-Periode
- [ ] Mixer Kernel-Thread: SCHED_FIFO, geweckt vom Audio-DMA-IRQ
- [ ] Deadline-Miss: Zero-fill (Silence) + Xrun-Counter pro Kanal
- [ ] HW-Input Kanaele (Mikrofon → Kanal)

## MIDI-Router

- [ ] MIDI IN/OUT Port pro Slot (dynamisch)
- [ ] Kernel MIDI-Router: Routing-Tabelle + Ringbuffer pro Verbindung
- [ ] Timestamped MIDI Events (sample-genau)
- [ ] /dev/midi (ALSA rawmidi kompatibel)
- [ ] Audio-Mixer: 24-Kanal Pool (siehe Audio-Mixer Sektion)

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
