# CosmoRT — Offene Punkte

Stand: 2026-03-30. ktest 1624/0. musl libc-test 454/18 (96.2%). LTP laeuft.

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

### RT-2: PI-Mutex (V2 erledigt)

- [x] rt_mutex Datenstruktur + API (lock, unlock, trylock)
- [x] Priority-Inheritance: Boosting + Deboosting (single-level)
- [x] Waiter-Array sortiert nach Prioritaet
- [x] Deadlock-Detection, Adaptive Spinning
- [x] **V2: Echtes Blocking** (kernel_setjmp/longjmp statt Spin-Wait)
- [x] FUTEX_LOCK_PI/UNLOCK_PI: cross-process shared futex Fix (epid)

### RT-3: Spinlock→mutex Konvertierung (abhaengig von RT-2 V2)

- [x] RT-3a: spinlock_t + mutex_t Infrastruktur
- [x] RT-3c: IPC (pipe, eventfd, timerfd, inotify, futex hash, sysv_ipc → mutex_t)
- [x] RT-3d: VFS/ext2 (ext2 fs_lock, bcache cache_lock → mutex_t)
- [x] RT-3e: Socket/Netzwerk (sock_lock, usock_lock, udp_table_lock → mutex_t)
- [x] RT-3f: Prozesse/MM (pid_lock, epoll_t::lock → mutex_t)
- NICHT konvertiert (IRQ-Kontext): eq_locks, core_rq, core_sleepers,
  rt_mutex::lock, process_t::lock (page fault), slab_t::lock,
  page_alloc buddy_lock, page_cache pc_lock, rng_lock, dmesg_lock

### RT-4: Threaded IRQs

Timer-IRQ bleibt Hard-IRQ (Scheduler-Heartbeat, nicht threadbar).
Device-IRQs → eigene Kernel-Threads (schedulebar, preemptibel, mutex-faehig).

#### RT-4a: IRQ-Thread Infrastruktur (erledigt)

- [x] irq_thread_create(name, handler_fn, prio): Kernel-Thread SCHED_FIFO
- [x] irq_thread_wake(thread): aus Hard-IRQ, weckt den Thread
- [x] Thread-Loop: blockiert → handler_fn() → blockiert (Endlosschleife)
- [x] Hard-IRQ Pattern: ACK + irq_thread_wake, sonst nichts
- [x] kthread_run im Scheduler: Kernel-Threads ohne Prozess/PML4
- [x] Magic Numbers in irq.c → VECTOR_* Defines
- [x] 12 ktest (irqt_*): Infrastruktur-Regression

#### RT-4b: Device-IRQs → Threads (erledigt)

- [x] NIC (e1000/virtio-net): net_thread (Paket-Dispatch, ARP/TCP/UDP)
- [x] Keyboard (hv_kbd/virtio-input): input_thread (Event-Ring, VT-Dispatch)
- [x] Serial: serial_thread (PTY-Bridge, on-demand wake)
- [x] rt_poll eliminiert (rt_poll.c/rt_poll.h geloescht)
- [x] timer_poll direkt aus timer_handler (kein rt_poll mehr)
- [x] TX-Wake: ip_send_raw weckt net_thread nach TX-Ring push
- [x] 11 ktest (tirq_*): Threaded-IRQ Validierung

#### RT-4c: Lock-Konvertierung Welle 2 (erledigt)

- [x] pty_t::lock → mutex_t (serial_bridge_poll weckt nur Thread, kein PTY-Zugriff aus Hard-IRQ)
- [x] tcp_hash_lock → mutex_t (tcp_input via net_thread, nicht Hard-IRQ)
- [x] tcp_rxring_t::lock → mutex_t (rxring_push/pop via net_thread)
- [x] pkt_queue_t::lock → mutex_t (q_push/q_pop via net_rx_one → net_thread)
- [x] 10 ktest (w2_*): PTY, TCP, pipe contention, mmap shared, file stress,
  socket, epoll multi-fd, unix socket, slab stress
- NICHT konvertiert (Analyse):
  - slab_t::lock: slab_grow_locked ruft pages_alloc (buddy_lock spinlock),
    mutex waehrend Boot ohne thread_current() verursacht GP fault
  - page_cache pc_lock: slab_free unter pc_lock → selbes Problem
  - process_t::lock: Page-Fault-Handler ist Thread-Kontext, aber zu breit gestreut
  - rng_lock: random_add_interrupt_entropy direkt aus timer_handler (Hard-IRQ)
  - dmesg_lock: serial_putchar aus Exception-Handlern und Panic-Pfaden
  - buddy_lock: page_alloc ueberall, zu riskant

### RT-5: RCU (erledigt)

- [x] RCU Datenstruktur (grace period, callback queue)
- [x] rcu_read_lock/rcu_read_unlock (preempt_disable/enable)
- [x] synchronize_rcu / call_rcu
- [x] rcu_check_quiescent in sched_preempt (Timer-Tick = QS)
- [x] ktest: 12 Tests (17 Checks) — Scheduling, Preemption, IPC intakt

### RT-6: NOHZ_FULL + Core-Isolation

Minimum 2 Cores. Kein Single-Core-Support.
Isolierte Cores: tickless, fuer Audio-Chains. Mehrere RT-Cores moeglich.

#### RT-6a: NOHZ_FULL Infrastruktur (erledigt)

- [x] LAPIC Timer Stop auf isolierten Cores (kein periodischer Tick)
- [x] One-Shot Timer fuer naechstes Wakeup (statt periodisch)
- [x] sched_preempt: auf tickless Cores nur via IPI/One-Shot (kein periodischer Tick)
- [x] rcu_check_quiescent: Idle-Pfad QS fuer tickless Cores (sched_loop)
- [x] epoll_check_timeouts: One-Shot-basiert bei sleeper_add auf tickless Cores
- [x] 12 ktest (nohz_*): NOHZ_FULL Validierung

#### RT-6b: Automatische Core-Isolation (erledigt)

- [x] Boot: BSP markiert Cores 2..N-1 als isoliert (Core 0 BSP, Core 1 Compute)
- [x] Scheduler migriert SCHED_FIFO/RR auf isolierte Cores (Round-Robin)
- [x] SCHED_OTHER automatisch ferngehalten (existiert bereits in sched.c)
- [x] Mehrere isolierte Cores fuer parallele Audio-Chains
- [x] 12 ktest (iso_*): Core-Isolation Validierung

### RT-7: CPU-Frequency-Invarianz (erledigt)

- [x] Invariant TSC Check (CPUID 0x80000007 EDX.8) beim Boot
- [x] tsc_khz / tsc_invariant Export fuer alle Subsysteme
- [x] Per-Core TSC Sync Check bei SMP-Boot
- [x] Alle Zeitmessungen via timer_tsc_now() (kein direktes rdtsc ausserhalb timer)
- [x] 15 ktest (fi_*): Timer-Precision, Monotonicity, Concurrent Timers

### RT-8: Validierung (erledigt)

- [x] Bounded WCET Audit auf allen Pfaden
- [x] Latenz-Messung + ktest Assertions (<10us WCET Ziel)
- [x] 15 ktest (rt_val_*): TSC-basierte Latenz-Messungen via clock_gettime
  Wake-to-Run, RT-Preemption, Signal-Delivery, Context-Switch,
  nanosleep Jitter/Overshoot, Timer-Fairness, Futex (un)contended,
  Pipe/eventfd Roundtrip, Fork, Preemption-under-Load, Yield, WCET-Smoke

---

## RT-9: Echtes schedule() (Kernel-Thread Blocking)

kernel_yield_jmpbuf ist nicht nestbar — IRQ-Thread-Loop und rt_mutex_lock
teilen sich denselben Buffer. Aktuell Workaround: kthreads spinnen bei
Mutex-Contention. Richtige Loesung: vollstaendiger Kernel-Stack-Context-Save
wie Linux/BeOS.

### RT-9a: Kernel-Stack Context-Save (Agent)

- [ ] sched_block(): speichert kompletten Kernel-Stack-Kontext (RSP, RBP, callee-saved)
- [ ] sched_resume(): stellt Kontext wieder her und springt zurueck
- [ ] Kein shared jmpbuf — jeder Block-Punkt hat eigenen Save auf dem Stack
- [ ] Funktioniert wie Linux __schedule(): switch_to() speichert/restored Kernel-RSP

### RT-9b: rt_mutex auf sched_block umstellen (Agent)

- [ ] rt_mutex_lock: sched_block() statt kernel_setjmp/longjmp
- [ ] rt_mutex_unlock: sched_wake restored den Blocked-Thread via sched_resume
- [ ] kthread Spin-Workaround entfernen
- [ ] Kernel-Threads koennen jetzt auf Mutexes schlafen

### RT-9c: IRQ-Thread-Loop auf sched_block umstellen (Agent)

- [ ] irq_thread_loop: sched_block() statt kernel_setjmp/longjmp
- [ ] kernel_yield_jmpbuf entfaellt (wird durch sched_block ersetzt)
- [ ] Nested Blocking funktioniert: IRQ-Thread blockiert in Loop, wacht auf,
  ruft Handler auf, Handler blockiert in mutex_lock — beides auf eigenem Stack

### RT-9d: event_wait auf sched_block umstellen (Agent)

- [ ] thread_block_ms: sched_block() statt kernel_yield
- [ ] event_wait Blocking-Pfad: sched_block()
- [ ] Einheitlicher Blocking-Mechanismus fuer den gesamten Kernel

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
