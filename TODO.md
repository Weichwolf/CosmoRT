# CosmoRT — Offene Punkte

Stand: 2026-03-30. ktest 1234/1. musl libc-test 454/18 (96.2%). LTP laeuft.

---

## Phase 0: Event-Driven Kernel (KRITISCH)

CosmoRT hat Busy-Wait-Polling an 18 Stellen. Das bricht Timer, Signale,
sleep/alarm/timeout und damit ALLE Tests die fork+wait oder sleep nutzen.
Alles muss event_wait() + Hardware-Timer (LAPIC) nutzen. Linux als Vorbild.

### P0-A: Timer (Fundament)

- [ ] timer_sleep_ms <10ms: LAPIC one-shot + arch_halt statt rdtsc spin-wait (timer.c:82)
- [ ] timer_sleep_ms >10ms: event_wait mit Timeout statt timer_ms() poll (timer.c:84-88)

### P0-B: Netzwerk-Boot (blockiert Scheduler)

- [ ] ARP resolve: event_wait auf ARP-Reply statt busy-wait loop (arp.c:210)
- [ ] DHCP discover: event_wait auf Paketankunft statt busy-wait loop (dhcp.c:40)
- [ ] DNS resolve: event_wait auf UDP-Reply statt busy-wait loop (dns.c:78)
- [ ] Boot DHCP in main.c: event_wait statt arch_halt loop (main.c:292)

### P0-C: SMP + TLB

- [ ] SMP AP boot: LAPIC timer statt pure spin-wait (smp.c:176) — CRITICAL, kein halt!
- [ ] TLB shootdown: LAPIC timer + Timeout statt 10M-Iteration spin-wait (irq.c:733)
- [ ] Kexec AP shutdown: LAPIC timer statt 2M-Iteration spin-wait (kexec.c:137)

### P0-D: IPC

- [ ] IPC receive: event_wait auf Endpoint statt 100-Iteration spin (ipc.c:133)
- [ ] IPC wait_any: event_wait auf ANY Endpoint statt Doppel-Scan (ipc.c:195)

### P0-E: Timerfd + Poll

- [ ] Timerfd expiry: Interrupt-Callback statt tight poll-loop in fd_poll_readiness (sys_ipc.c:461)
- [ ] Socket/epoll deadline checks: TSC-basiert statt timer_ms() vor event_wait

---

## Phase 1: Signal-Delivery (abhaengig von P0)

- [x] SIGSTOP/SIGCONT remote: sofortiger Stop + preempt-safe
- [x] SIGALRM: per-core Alarm-Check + BSP-Scan
- [x] SIGTERM/SIGKILL: event_wait prueft vor Block
- [x] Preempt-safe Kill: SIG_DFL in Timer-IRQ ohne Frame-Corruption
- [x] Remote Kill: sched_wake auf blockierte Threads
- [x] SIGCHLD: nicht in check_pending_signals clearen (sigtimedwait braucht es)
- [x] nanosleep restart: Deadline erhalten ueber Signal-Unterbrechung
- [ ] nanosleep re-block: nach Signal-Delivery verbleibende Zeit weiterschlafen
- [ ] SIGALRM an blockierte Prozesse: event_wait muss ALLE Signale pruefen (nicht nur SIGKILL/SIGTERM)

---

## Phase 2: Shared Memory + Cross-Process

- [x] MAP_SHARED fork: physische Pages teilen (nicht COW)
- [x] Shared Futex: non-PRIVATE pid=0 fuer cross-process Hash
- [ ] MAP_SHARED anonymous coherency: ktest map1/map2 data
- [ ] MAP_SHARED file-backed: Page Cache Coherency zwischen Prozessen
- [ ] /dev/shm tmpfs: ramfs Hard Links (implementiert), mmap coherency (offen)

---

## Phase 3: LTP Test-Compliance

### Bestanden (10 PASS)

- [x] abort01: WCOREDUMP Bit fuer Core-Dump-Signale
- [x] accept01: EOPNOTSUPP fuer UDP accept
- [x] accept03: ENOTSOCK fuer Non-Socket FDs, EBADF fuer O_PATH
- [x] access02, access03: Basis-Existenzpruefung
- [x] alarm02, alarm03, alarm05, alarm06: alarm + sleep + cancel

### Fehlend — nach Prioritaet

- [ ] accept02, accept4_01: Loopback TCP accept (SYN in q_tcp nicht verarbeitet)
- [ ] access01, access04: Permission-Bits pruefen (chmod/fchmod enforcement)
- [ ] acct01: Process Accounting (ENOSYS ok?)
- [ ] adjtimex01-03: adjtimex Modes + Validation (teilweise implementiert)
- [ ] alarm07: fork + sleep(3) + alarm — nanosleep re-block Bug

---

## Phase 4: musl libc-test Remaining (18 FAIL)

- [ ] fma, fmal, powf, remquol: FPU Precision (QEMU softfloat?)
- [ ] pthread_robust (2): robust mutex Timeout (exit cleanup implementiert)
- [ ] sem_open (2): shared mmap coherency (initial value)
- [ ] socket (2): loopback TCP accept
- [ ] utime (2): Y2038 (ext2 uint32 timestamps)
- [ ] pthread_atfork (2): errno clobber bei fork in multithreaded
- [ ] rlimit-open-files (2): RLIMIT_NOFILE enforcement (implementiert, Test erwartet anderes)
- [ ] malloc-brk-fail (1): brk OOM recovery
- [ ] tls_get_new-dtv (1): dynamic TLS segment allocation
- [ ] raise-race (2): fork in Signal-Handler + RT signals (geskippt)

---

## Alpine Kompatibilitaet — Blocker

### IPC

- [ ] inotify echte Events (IN_CREATE, IN_MODIFY, IN_DELETE)
- [ ] SCM_RIGHTS: fd-Passing ueber Unix Socket
- [ ] Abstract Unix Sockets (@ Namespace)
- [x] /dev/shm: ramfs Hard Links fuer sem_open
- [ ] flock echtes Advisory Locking
- [ ] SysV IPC: shmget/shmat/shmctl, semget/semop/semctl, msgget/msgsnd/msgrcv/msgctl

### Prozesse/Threads

- [ ] CLONE_THREAD korrekt: echte Threads (gleiche PID, verschiedene TIDs)
- [ ] sendfile: zero-copy file transfer
- [ ] getrusage korrekt: CPU-Zeit pro Prozess/Thread

### procfs

- [ ] /proc/self/fd/ Directory Listing
- [ ] /proc/pid/stat komplett
- [x] /proc/PID/oom_score_adj: read + write
- [ ] /proc/sys/kernel/threads-max

---

## Security

### Sec-E: Spectre/Meltdown

- [ ] KPTI + PCID
- [ ] Retpoline + IBRS/IBPB
- [ ] SSBD + MDS VERW

---

## Netzwerk

- [x] Loopback TCP: Block in socket.c entfernt, ARP-Bypass
- [ ] Loopback TCP accept: SYN aus q_tcp verarbeiten
- [ ] IPv6 Basis (RFC 8200) + NDP + SLAAC

---

## RT/Compute — ARINC 653

- [ ] ARINC-A: Shared Locks eliminieren
- [ ] ARINC-B: Dynamic Alloc auf RT-Core eliminieren
- [ ] ARINC-C: IPI-Isolation
- [ ] ARINC-D: Bounded Data Structures
- [ ] ARINC-E: RT-Core Memory-Isolation

---

## Device Nodes (SDL3/UI)

- [ ] /dev/fb0, /dev/input/event0, /dev/snd/, /dev/dri/

---

## Skalierung (niedrige Prio)

Skal-G..N: PTY Pool, epoll_ctl Hash, Unix Socket Slab, etc.

---

## Fehlende Implementierungen

- [ ] ARCH_SET_GS/ARCH_GET_GS
- [x] prlimit64 set (RLIMIT_NOFILE)
- [ ] sendfile, splice/tee/vmsplice
- [ ] Hard Links in ext2
- [ ] renameat2 RENAME_EXCHANGE
- [x] Robust Mutex: set_robust_list + Thread-Exit-Cleanup
- [x] adjtimex: Mode-Validation + ADJ_SETOFFSET reject
- [x] WCOREDUMP Bit in wait-Status
- [x] O_PATH in fd flags
- [x] futimens(fd): utimensat mit NULL path
- [x] utimensat UTIME_NOW/UTIME_OMIT
- [x] OOM Guard: mmap/brk Headroom
