# CosmoRT — TODO

**Ziel**: POSIX/Linux-kompatibler RT-Kernel, x86_64 + aarch64, ohne 30-Jahre-Legacy.
Vision: was Linus heute bauen würde auf moderner Hardware.

**Stand (Session-Ende)**: ktest **2906/1** (CLONE_NEWNET pre-existing
fail), musl 460/11, **LTP 247/7/44** (+3 PASS / -3 FAIL vs Phase-10-Stand).
Phase 11 erledigt: clock_nanosleep01 PASS via apply_restart() im
syscall-return-Pfad + ERESTART_RESTARTBLOCK in do_clock_nanosleep +
restart_block in thread_t. Branch: `ltp`.

**Strukturelle Blocker für 100% grün** (siehe Analyse): kein Waitqueue-System,
kein `restart_block`, Scheduler UP-designed mit SMP-Patches obendrauf.
Ohne diese Fundamente bleiben Signal-Handling, Timer-Präzision und
SMP-Stabilität brüchig.

---

## Roadmap

| # | Phase | Kern-Problem | Aufwand | Blocker für |
|---|-------|--------------|---------|-------------|
| **10** | **Waitqueue-System** | alle Wait-Pfade eigener ad-hoc Code | ~1500 LOC Big-Bang | 11, 13 |
| **11** | **restart_block + signal-restartable syscalls** | EINTR ohne rem-Recovery | ~500 LOC | 12 |
| **12** | **hrtimer ns-Präzision + Tick-less** | timer_ms() in Hot-Path | ~800 LOC | 14 |
| **13** | **SMP-saubere Scheduler-Finalisierung** | globales rq_lock | ~1200 LOC | 15 |
| **14** | **vDSO clock_gettime** | syscall pro clock_gettime | ~600 LOC | — |
| **15** | **Network-Namespaces** | netif+route-table global | ~2000 LOC | 16, 17 |
| **16** | **IPv6-Stack** | AF_INET6 = EPROTONOSUPPORT | ~2500 LOC | 17 |
| **17** | **OOM-Killer + oom_score_adj** | alloc-fail → -ENOMEM ohne Reclaim | ~600 LOC | — |
| **18** | **aarch64-Port** | HAL-Stubs nur x86_64 real | mehrere Sessions | Multi-Arch |
| **19** | **Audio-Subsystem** | RT-Audio-Identität | Neubau | CosmoRT-USP |

Reihenfolge ist **nicht** verhandelbar bis 15 — danach orthogonal parallelisierbar.

---

## Phase 10 — Waitqueue-System (KRITISCHER PFAD)

**Status**: Infrastruktur (10.1) + futex + pipe auf waitqueue (10.2a/b).
Rest (event_wait/net/epoll/signalfd/timerfd/process_wait) offen als
Phase 10.2c.

**Problem**: Jede Blocking-Primitive (`thread_block_ms`, `event_wait`, `futex_wait`,
`signalfd_read`, socket-recv, pipe-read) baut ihr eigenes
`state=BLOCKED → schedule()`. Atomarität zwischen State-Transition und
Queue-Insertion fehlt. Sched_wake's CAS hat Race-Window vor state=BLOCKED.

**Symptome die aufgedeckt sind**:
- clock_nanosleep01/02 TBROK (3× reverted, siehe git log)
- pthread_mutex_pi-Flakes
- sem_init-Hang bei Signal-Wake-Patches
- accept03-Hang bei Signal-Wake-Patches
- pthread_cond-smasher-Flake
- udiv-Timeout nach Signal-Änderungen
- pthread_once-deadlock
- sscanf_long-Hang bei Signal-Wake-Patches

**Alle haben dieselbe Wurzel**: Wake-Signal zwischen "condition check" und "sleep".

### Scope

**Phase 10.1 — Infrastruktur (ERLEDIGT)**

- [x] `include/kernel/core/waitqueue.h` — `wait_queue_head_t` + `wait_queue_entry_t`,
      `DEFINE_WAIT`, `init_waitqueue_head`, `add_wait_queue`, `remove_wait_queue`
- [x] `src/kernel/core/waitqueue.c` — `prepare_to_wait(wq, wait, state)`,
      `finish_wait(wq, wait)`, `wake_up(wq)`, `wake_up_one(wq)`,
      `wake_up_interruptible(wq)`
- [x] Lock-Semantik: waitqueue hat eigenen Spinlock, state-Transition +
      queue-Insertion unter diesem Lock. Waker sperrt denselben → keine Missed-Wakeups.
- [x] Exclusive-Wakeups (ein Waiter pro Signal) via `WQ_FLAG_EXCLUSIVE`
- [x] Signal-interruptible: schedule_timeout_interruptible checkt `sig_pending`
- [x] `schedule_timeout`/`schedule_timeout_interruptible` als Primitiv, ns-Praezision
- [x] `sleep_interruptible_ns` ersetzt nackten state=BLOCKED+schedule()-Loop
- [x] `thread->wait_head/wait_entry` Pointer, `sched_wake` routet ueber wait_head
- [x] sched_wake DEAD-guard verhindert UAF auf recycelten kstack
- [x] kill_one nutzt sched_wake-direct fuer waitqueue-parked threads (keine
      stale EQ-Events mehr)
- [x] 16 neue ktests: short/zero/repeated/abstime sleep, signal-interrupts-sleep,
      SIGTERM/SIGKILL during sleep, pipe block, wait4 wake, 20 concurrent sleepers,
      30x signal-wake stress, 5 alternating sleeps

**Phase 10.2a — futex auf waitqueue (ERLEDIGT, ef2994d)**

- [x] `futex_wait`/`futex_wake` → wait_queue_head_t pro bucket
- [x] FUTEX_LOCK_PI/UNLOCK_PI auf gleiche Infrastruktur
- [x] FUTEX_WAITER_MAX=256 slab entfernt — stack-allocated via
      DEFINE_WAIT. Kein systemweites Pool mehr, Prozess kann nur
      seinen eigenen Kernel-Stack erschoepfen.
- [x] FUTEX_REQUEUE transplantiert entry zwischen buckets + re-bindet
      thread->wait_head atomar.
- [x] Neue ktests: `futex_bucket_key_filter` (2 keys, selber bucket),
      `futex_wake_n` (wake genau N von 5 waiters).

**Phase 10.2b — pipe auf waitqueue (ERLEDIGT, 0277e99)**

- [x] `pipe_read_blocking`/`pipe_write_blocking` → DEFINE_WAIT_EXCLUSIVE
      auf wq_readers/wq_writers pro pipe. Exclusive wake — nur einer
      pro write. Kein thundering herd.
- [x] Single-blocker-pointer `blocked_reader/writer` geloescht; beliebig
      viele parallele Waiter moeglich.
- [x] `pipe_close` ruft `wake_up_all` zum EOF/EPIPE-Broadcast.
- [x] Neue ktests: `pipe/two_readers_exclusive_wake`,
      `pipe/close_broadcasts_eof`.

**Phase 10.2c — Restliche Blocking-Pfade**

- [x] `event_wait_ns` → waitqueue auf `event_queue_t` (58c3b93, 1a3d33a,
      828b55e, da79de0). prepare_to_wait/finish_wait haengen Consumer
      atomic an `eq->wq`; event_post weckt via wake_up_interruptible.
      Schliesst den Race der Phase-12-tickless sem_init/tls_init-Hangs
      ausgeloest hat. +9 ktest Sub-Asserts (event_wait_race/01..05).
- [ ] `signalfd_read` → waitqueue pro signalfd
- [ ] socket recv/accept → waitqueue pro socket (TCP/Unix nutzen
      event_wait — profitiert indirekt schon von 10.2c)
- [ ] epoll_wait → eigene waitqueue + ep_poll_callback pro registriertem
      fd (heute event_wait-getragen, indirekter Fix via 10.2c)
- [ ] `process_wait`/`wait4` → waitqueue pro process fuer SIGCHLD

Hinweis 10.2c: wait4-Migration wurde angefangen (child_wait_wq auf
process_t plus wake_up_interruptible in den exit/stop/continue-Pfaden)
und wegen PID_TABLE_GROWTH-Hang wieder verworfen. Die Signal-
Interlocking-Logik zwischen event_post und wait_queue braucht
separate Analyse — vermutlich Lock-Ordnung zwischen parent->lock und
child_wait_wq.lock oder eq_lock-Rekursion. Phase 10.2c nimmt das an.

### Erfolgskriterien

- ktest +20 (Waitqueue-Tests)
- **musl pthread-Flakes eliminiert** (sem_init, pthread_mutex_pi,
  pthread_cond-smasher, pthread_once-deadlock, udiv): alle ≥99% PASS-Rate
- **LTP nanosleep01/02 PASS** (sobald Phase 11 draufsitzt)
- `-smp 2` nicht mehr Russian-Roulette

### Risiken

- **Big-Bang nötig** — halbe Umstellung gibt gemischten Zustand + Deadlocks.
  Wenn Agent unterbrochen wird: Branch verwerfen, neu starten.
- Interaktion mit RCU-lock (Phase 7.5): add/remove_wait_queue muss
  RCU-safe sein falls Waiter während Wake entfernt werden.

---

## Phase 11 — restart_block + signal-restartable syscalls (ERLEDIGT)

**Status**: clock_nanosleep01 PASS. Net LTP +3 PASS (244→247) /
-3 FAIL. ktest 2906 (+2 vom Phase-11-Testset; CLONE_NEWNET pre-existing
fail). futex_wait-Migration verworfen (pthread_mutex_pi-static
Regression — eigene Boost-Rollback-Semantik).

- [x] `struct restart_block` in `thread_t`: function-ptr + 6 long args
- [x] `SYS_restart_syscall` (219) — ruft `current->restart_block.fn(block)`
- [x] `do_nanosleep` / `do_clock_nanosleep`: setzt
      `restart_block.fn = clock_nanosleep_restart`, verbleibende Zeit
      in args, returnt `-ERESTART_RESTARTBLOCK`
- [x] syscall-return konvertiert `-ERESTART_*` -> EINTR oder
      RIP-rewind+RAX=orig_num bzw. RAX=219 (apply_restart, Linux
      do_signal-konform)
- [x] SA_RESTART-Behandlung: `-ERESTARTSYS` -> rewrite auf original nr
- [x] read/wait4/pipe/socket: ERESTARTSYS statt direktem EINTR
- [x] 8 ktests: nanosleep + SIGUSR1, SA_RESTART, default-IGN,
      TIMER_ABSTIME+SIG, futex_wait+SIG, pipe-read+SIG, wait4+SIG,
      wait4+SA_RESTART
- [ ] do_futex(FUTEX_WAIT) restart_block — verworfen wegen Regression
      (pthread_mutex_pi-static); revisit nach Phase 13 SMP-Stabilisierung
- [ ] do_poll/do_select restart_block — Phase 12 (hrtimer)

### Bilanz

- **LTP clock_nanosleep01 PASS**
- LTP clock_nanosleep02: timing-basiert (500x 1ms, expects <30s) ->
  Phase 12 (hrtimer ns-Praezision)
- musl pthread_atfork-errno-clobber: bleibt FAIL (Test prueft
  errno-Erhaltung ueber Signal-Trampolin, andere Wurzel)

---

## Phase 12 — hrtimer ns-Präzision + Tick-less

**Problem**: `timer_ms()` rundet auf ms, Ticks bei 1000Hz (1ms). Zu grob für:
- `epoll_wait02` (500×epoll_wait(1ms), erwartet <2s)
- `clock_gettime04` (aufeinanderfolgende clock_gettime >5ms diff)
- precision-sleep unter 1ms
- RT-Audio-Deadline (<250µs für 48kHz stereo)

### Scope

- [ ] `hrtimer_now_ns()` als ein-Funktions-Aufruf-Hot-Path, TSC-direct
- [ ] `HZ_ns` = 1_000_000 für default-tick (1ms), konfigurierbar
- [ ] **Tick-less**: statt periodischer Tick ein one-shot LAPIC-Timer
      auf nächste Deadline (min(sched-quantum-expire, hrtimer-expire))
- [ ] `schedule_timeout(ns)` nutzt hrtimer statt tick-count
- [ ] TSC-calibration bei boot + Hyper-V TSC-page reference
- [ ] `clock_gettime(CLOCK_MONOTONIC)`-Kernel-Pfad: TSC-read + ns-scale
      (ohne syscall-Overhead wird erst Phase 14 erreicht)
- [ ] ktests: ns-precision-sleep, hrtimer-reprogram-race, tickless-idle

### Erfolgskriterien

- LTP epoll_wait02 PASS
- LTP clock_gettime04 PASS
- musl nanosleep-precision-Tests durchlaufen <2% jitter

---

## Phase 13 — SMP-saubere Scheduler-Finalisierung

**Problem**: Globales `rq_lock` + `rq_bitmap` in `sched.c`. `sched_wake` CAS
auf `thread->state` hat Race mit state=BLOCKED-Setup (siehe Phase 10,
die macht das obsolet via waitqueue — aber der Rest bleibt).

### Scope

- [ ] `struct rq` per-CPU (nicht global)
- [ ] `sched_wake(task)` auf Task's rq (home-CPU), nicht current-CPU
- [ ] Work-Stealing: idle-CPU stiehlt runnable-Task von überlasteter rq
- [ ] Load-Balancing-Tick (alle N ms rebalance)
- [ ] CPU-Affinity-Support (sched_setaffinity), Task-Migration
- [ ] IPI-basierter preempt-trigger für remote wake
- [ ] ktests: multi-CPU wake, affinity-pinning, work-stealing

### Erfolgskriterien

- ≥8 CPUs (`-smp 8`) stabil in alpine-test
- accept03 nicht mehr flake
- clock_gettime03 nicht mehr flake

### Abhängigkeit

**Benötigt Phase 10** (waitqueue-System) — sonst reproduziert rq-Refactor
die gleichen Missed-Wakeups pro-CPU.

---

## Phase 14 — vDSO clock_gettime (ERLEDIGT)

**Status**: ELF-vDSO als 4KB-DSO embedded, mapped in jede init-time_ns
process-mm, AT_SYSINFO_EHDR im AUXV, musl findet __vdso_clock_gettime
via dynsym. ktest 2939 -> 2951 (+12), musl 461/10 (+1 PASS).

### Implementiert

- [x] ELF-vDSO-Page (build/user/vdso.so, 2208 bytes, 1 page LOAD seg)
- [x] vdso_data struct (64-byte cacheline, seqlock, mult/shift/boot_tsc)
- [x] vdso_init() bei boot — allocates phys pages, copies ELF, populates data
- [x] vdso_map(pml4, &vma_root) — RO data + RX code in user-mm + MAP_VDSO VMAs
- [x] vdso_unmap() in free_address_space — kernel-owned pages survive exit
- [x] AT_SYSINFO_EHDR im AUXV (process_exec.c::build_user_stack)
- [x] musl resolviert __vdso_clock_gettime via versioned LINUX_2.6 dynsym
- [x] CLOCK_MONOTONIC + CLOCK_REALTIME + COARSE-Varianten im vDSO
- [x] vdso_data_update() bei clock_settime + tsc_recalibrate (weak link)
- [x] crt0.S kapselt argv/envp/auxv in __crt0_argc/argv/envp/auxv globals
- [x] 12 ktests: AT_SYSINFO_EHDR, ELF magic, dynsym walk, CLOCK_MONOTONIC,
      vdso vs syscall delta, CLOCK_REALTIME, monotonic over 1000 calls,
      1000 calls < 10ms (measured ~88us → ~88ns/call)

### Bilanz

- ktest 2951 (+12 vDSO sub-asserts), test-hw clean (1 pre-existing FAIL)
- musl libc-test 461 PASS (+1) / 10 FAIL / 7 SKIP
- LTP clock_gettime04: bleibt FAIL — TSC granularity in QEMU + 5ms test
  budget bei 6 clk_ids * 10000 iterations * 5 variants (300k Aufrufe).
  vDSO senkt per-call Latency von ~150ns (syscall) auf ~88ns (vDSO),
  reicht im qemu-Setup nicht ueber den Threshold.
- LTP clock_nanosleep01: pre-existing hang (kein Phase-14 Regression)

### Time-namespace-Integration (offen, dependency Phase 15)

- vDSO-Daten pro-NS — heute: vdso_map skipped fuer non-init time_ns,
  musl faellt zurueck auf syscall (kernel adjustiert offsets dort
  korrekt). Vollstaendige per-NS vDSO data pages = Phase 15.

### Bekannte Limits

- Single global vdso_data — alle Prozesse mit init_time_ns teilen
  TSC-mult/shift + wall_time_offset. Per-NS Daten erfordern separates
  data page pro time_namespace (Linux model).
- vDSO-Image nur 4KB, fuer __vdso_clock_gettime + __vdso_getcpu
  ausreichend. __vdso_gettimeofday + __vdso_time TODO.

---

## Phase 15 — Network-Namespaces

**Problem**: CLONE_NEWNET liefert EINVAL. netif+route-table+socket-lookup
sind singleton. clone09 TBROK.

### Scope

- [ ] `struct net_ns` mit netif-list, route-table, socket-hash, conntrack-stub
- [ ] `task_struct.net_ns` Pointer, fork inherit, unshare/setns
- [ ] netif-APIs (`netif_add`, `netif_find`, routing) alle ns-scoped
- [ ] socket-lookup-key: (family, src-port, dst-port, src-addr, dst-addr, **ns-id**)
- [ ] /proc/self/ns/net symlink, setns(fd, CLONE_NEWNET)
- [ ] /proc/sys/net/ipv4/conf/{lo,default}/* per-ns
- [ ] ktests: multi-ns-isolation, port-reuse-different-ns, lo-per-ns
- [ ] loopback pro-ns (CosmoRT.lo im root-ns, kernel-ns-lo im neuen ns)

### Erfolgskriterien

- LTP clone09 PASS
- LTP netns-Tests (~10 weitere) laufen oder sauber SKIP
- 2 Prozesse mit eigenem NS binden beide Port 8080 ohne Konflikt

### Abhängigkeit

**Entkoppelt von Phase 10-14** — kann theoretisch parallel laufen, aber
Scope+Risiko sprechen für sequentiell nach 13.

---

## Phase 16 — IPv6-Stack

**Problem**: AF_INET6 → EPROTONOSUPPORT. Blockiert bind04/IPv6-subcase,
connect02-family, ~20 LTP net-Tests die aktuell SKIP.

### Scope

- [ ] `struct in6_addr` + `struct sockaddr_in6` (Linux-exakt)
- [ ] IPv6-Header-Parsing (40 Byte fixed + Extension-Headers)
- [ ] ICMPv6 (NS/NA/RS/RA/Echo)
- [ ] Neighbor Discovery (ersetzt ARP für IPv6)
- [ ] DAD (Duplicate Address Detection)
- [ ] SLAAC Address-Autoconfig
- [ ] IPv6-Routing-Table (pro netns, siehe Phase 15)
- [ ] TCP6, UDP6 — socket-state maschinen sind IPv4-identisch, nur addr-Felder ändern
- [ ] Socket-Lookup-Hash: 128-bit addr + port, Hash-Funktion angepasst
- [ ] getaddrinfo()-Kernel-Fallback (musl nutzt /etc/hosts)
- [ ] Dual-Stack: AF_INET-socket empfängt IPv4-mapped-IPv6 wenn `IPV6_V6ONLY=0`
- [ ] ktests: SLAAC, NDP, tcp6-connect, udp6-echo

### Erfolgskriterien

- LTP bind04 komplett PASS (alle tcases inkl. IPv6)
- LTP connect02, ping6, socket-Tests laufen ohne SKIP
- ping6 ::1 funktioniert

### Abhängigkeit

**Benötigt Phase 15** — sonst IPv6 direkt wieder singleton.

---

## Phase 17 — OOM-Killer + oom_score_adj (ERLEDIGT)

**Status**: ktest 2906 -> 2939 (+33), commits 80f648a..50e992c.
musl 460/11 unveraendert, LTP 247/7/44 unveraendert.
out_of_memory() fuerte live wahrend cve-2017-17052: "oom: killing
pid 21 score 986" → Test PASS (vorher Flake je nach Memory-Druck).

### Implementiert

- [x] `task_struct.oom_score_adj` int16, -1000..1000, fork inherit
- [x] `task_struct.oom_score_adj_min` (CAP_SYS_RESOURCE-Gate fuer
      Lowering past min, monotonisch)
- [x] `/proc/$pid/oom_score_adj` (rw)
- [x] `/proc/$pid/oom_score` (ro, berechnet via `oom_badness`)
- [x] `/proc/$pid/oom_adj` (legacy -17..15, scaled, OOM_DISABLE special-case)
- [x] `out_of_memory()` an page_alloc/pages_alloc/huge_page_alloc-fail:
  - score alle tasks: (rss + pgtables) * 1000 / total + adj * total / 1000
  - highest-score: SIGKILL + retry-once nach schedule()
  - immune wenn adj == OOM_SCORE_ADJ_MIN
  - Re-Entry-Guard: per-CPU oom_in_progress[], plus context-check
    (in_preempt, current_thread) — kein OOM aus IRQ/preempt
- [x] init (pid 1) ist immune (panic statt kill)
- [x] SUID/SGID-exec resetet oom_score_adj{,_min} auf 0 wenn euid != ruid
      und kein CAP_SYS_RESOURCE
- [x] ktests: 9 neue (clamp, inherit, min-gate, legacy-roundtrip,
      score-sanity, init-pid1, exec-preserve, pick-highest-adj,
      tracks-rss)

### Bilanz

- ktest 2906 -> 2939 (+33 sub-asserts, target +6 deutlich uebertroffen)
- /proc/self/oom_score_adj read/write: LTP-Setup-Pfade die bisher
  "oom_score_adj does not exist, skipping" geloggt haben finden
  jetzt den Knoten.
- musl/LTP-Baseline keine Regression.

---

## Phase 18 — aarch64-Port

HAL-Stubs bereits in `src/arch/aarch64/hal_*.c` (Phase 7.2, abgeschlossen).
Interface-Oberfläche aarch64-generic. Jetzt echte Implementierungen.

### Scope

- [ ] `boot/entry.S` EL2→EL1 Drop, Stack-Init
- [ ] `cpu/exceptions.c` ESR-Decode, Vector-Table
- [ ] `mm/paging.c` 4K Pages, 3-Level PT, TLBI
- [ ] `irq/gic.c` GICv2/v3
- [ ] `timer/arch_timer.c` Generic Timer CNTV_CTL
- [ ] `smp/smp.c` PSCI CPU_ON (statt Trampoline <1MB)
- [ ] `syscall/entry.S` SVC #0 Pfad
- [ ] `context.S` x0-x30 + FPSIMD
- [ ] syscall-Tabelle x86_64 ≠ aarch64 → eigene Tabelle
- [ ] Boot-Test: QEMU aarch64 virt-Machine
- [ ] Alpine aarch64 rootfs bauen, alle musl+LTP Tests dort auch grün

### Erfolgskriterien

- `make ARCH=aarch64 alpine-test` alle musl+LTP PASS wie x86_64
- Shared-ABI-Regressionen zwischen Archs <1%

### Abhängigkeit

**Profitiert von Phase 10-14** — ohne waitqueue + ns-Präzision wird aarch64
dieselben Flakes zeigen.

---

## Phase 19 — Audio-Subsystem (CosmoRT-USP)

**Vision**: BeOS-inspiriertes Audio-Subsystem. Nicht ALSA-kompatibel
(Legacy), sondern neu: RT-Deadline-scheduler-integriert, zero-copy
bus-routing, plugin-native, sample-accurate.

### Scope (Initial)

- [ ] `struct audio_stream` — 32-bit float, ≥96kHz, variable framesize
- [ ] `audio_dev` HAL (hda + virtio-snd + usb-audio)
- [ ] Mixer-Bus mit node-graph (wie BeOS BMediaNode)
- [ ] RT-priority-class für audio-threads (SCHED_DEADLINE)
- [ ] /dev/snd/{controlC0,pcmC0D0p} Linux-kompatible-API für Alpine-Tools (alsa-utils)
- [ ] JACK-kompatible-Socket-API für jackd-client-Apps
- [ ] Plugin-API (LADSPA? oder eigenes)
- [ ] ktests: underrun-detection, xrun-latency, sample-accurate-sync

### Erfolgskriterien

- `aplay test.wav` funktioniert in Alpine
- Sub-ms-Latency under Kernel-Stress
- 8 parallel streams ohne dropout

### Abhängigkeit

**Benötigt Phase 12** (hrtimer-ns) und **Phase 13** (SMP-sched).

---

## Parkplatz (nach Phasen-Abschluss neu-evaluieren)

### LTP-SKIP-Audit (#70)
Alle ~44 SKIPs auditieren: Typ A (legitim) vs Typ B (versteckter FAIL
wegen fehlender Kernel-Features). Macht Sinn **nach Phase 15/16**, weil
dann Netns + IPv6 einen Großteil der SKIPs aus Typ B holen.

### Dokumentierte Flakes
Mit Phase 10 sollten verschwinden:
- accept02, accept03 (timing)
- fcntl36/36_64 (OFD-Race)
- pthread_mutex_pi, sem_init, sscanf_long, udiv (signal-wake)
- leapsec01, cve-2017-17052 (timing/memory-stress)

---

## Session-Bilanz (Session-Ende)

| Metrik | Start-of-Session | Ende |
|---|---|---|
| ktest | 2504 | **2870** (+366) |
| ktest FAIL | variable | **0** |
| musl PASS | 448 | **460** (+12) |
| LTP PASS | 198 | **246** (+48) |
| LTP FAIL | 87 | **8** (-79) |

### Gefixte Cluster (Session)
fcntl (komplett), eventfd/epoll, chroot/caps, cve/execve, clone,
clock_*/adjtimex, bind/accept (TCP half-open, path-resolve, SEQPACKET),
access01/04 (DAC), fcntl35/35_64 (procfs-write), fcntl38/38_64 (nested
sigreturn), rest-bucket (8/9), dup201, copy_file_range03, CLONE_NEWNET.

### 3× reverted (dokumentiert)
Signal-Wake in thread_block_ms (direct-assign → CAS → wakeup_pending-flag).
Alle 3 reproduzierten den Missed-Wakeup-Race auf andere Art. Phase 10
(Waitqueue-System) ist die nachhaltige Lösung.

---

## Regeln

**Eine Phase, eine Transaktion.** Halbe Umstellung ist gefährlicher
als gar keine (siehe 3× Signal-Wake-Revert).

**Jeder neue Code-Pfad bekommt einen ktest** (CLAUDE.md).
ktest-Count muss monoton wachsen, nicht nur stabil bleiben.

**Linux-ABI ist nicht verhandelbar.** Jede Abweichung ist ein Bug.
