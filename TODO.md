# CosmoRT — TODO

**Ziel**: POSIX/Linux-kompatibler RT-Kernel, x86_64 + aarch64, ohne 30-Jahre-Legacy.
Vision: was Linus heute bauen würde auf moderner Hardware.

**Stand (Session-Ende)**: ktest **2911-2915/1** (CLONE_NEWNET pre-existing,
4 Tests flake je nach Run). pthread_mutex_pi + pthread_mutex_pi-static
PASS nach WAITERS-bit-unter-lock-fix (4d9ab0c). pthread_cond-static
triggert Kernel-PF (CR2=0x10) — pre-existing Bug unabhaengig von 10.2,
Reihenfolge im Debug-Stack-frame. Full alpine-test in dieser Session
nicht durchgelaufen (Timeouts + log-Kollisionen), aber alle migrierten
ktests gruen. Branch: `ltp`.

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

**Phase 10.2c — Restliche Blocking-Pfade (OFFEN)**

- [ ] `event_wait` → waitqueue pro `event_queue_t` (Kooperation mit event_post)
- [ ] `signalfd_read` → waitqueue pro signalfd
- [ ] socket recv/accept → waitqueue pro socket
- [ ] epoll_wait → waitqueue + ep_poll_callback pro registriertem fd
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

## Phase 11 — restart_block + signal-restartable syscalls

**Problem**: Unterbrochene `clock_nanosleep`/`futex`/`poll`/`select` liefern
`-EINTR`, aber Linux-Spec verlangt restart_block: der Kernel speichert
eine Resume-Closure, und der Userspace-Wrapper ruft bei `ERESTART_RESTARTBLOCK`
automatisch `SYS_restart_syscall` (nr=219) auf. Das macht Sleeps und
Waits atomar gegenüber SA_RESTART-Signalen.

### Scope

- [ ] `struct restart_block` in `thread_t`: function-ptr + 6 long args
- [ ] `SYS_restart_syscall` (219) — ruft `current->restart_block.fn(block)`
- [ ] `do_nanosleep` / `do_clock_nanosleep`: setzt `restart_block.fn =
      clock_nanosleep_restart`, verbleibende Zeit in args, returnt
      `-ERESTART_RESTARTBLOCK`
- [ ] syscall-entry konvertiert `-ERESTART_RESTARTBLOCK` → rewrite syscall-nr
      auf 219, re-dispatch nach Handler-Return
- [ ] SA_RESTART-Behandlung: `-ERESTARTSYS` → rewrite auf original nr
- [ ] `do_futex(FUTEX_WAIT)` → restart_block mit abs-deadline
- [ ] `do_poll`/`do_select` → restart_block mit `remaining_time_ns`
- [ ] ktests: SIGINT während nanosleep → rem korrekt; SIGUSR1 mit SA_RESTART
      während futex_wait → wake handler, resume wait

### Erfolgskriterien

- **LTP clock_nanosleep01/02 PASS** (zusammen mit Phase 10)
- musl pthread_atfork-errno-clobber PASS (braucht SA_RESTART-konform)
- Keine Regression auf SIGINT-Handler-Tests

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

## Phase 14 — vDSO clock_gettime

**Problem**: clock_gettime macht syscall → 100-200ns Overhead pro Call.
LTP clock_gettime04 macht 6000 Iterationen mit <5ms Budget — jede
syscall-TLB-Flush kostet uns.

### Scope

- [ ] ELF-vDSO-Page bei boot allozieren, in jede process-mm mappen
- [ ] `__vdso_clock_gettime` in Assembly/C: TSC-read + ns-scale + Offset aus
      Userspace-sichtbarer `struct vdso_data`
- [ ] kernel schreibt `vdso_data` bei jedem TSC-sync / time_ns-Switch
- [ ] musl findet vDSO via AT_SYSINFO_EHDR auxv-Entry
- [ ] CLOCK_MONOTONIC, CLOCK_REALTIME, CLOCK_MONOTONIC_RAW im vDSO
- [ ] ktests: cycle-count clock_gettime < 50ns
- [ ] time-namespace-Integration: vDSO-Daten pro-NS (siehe Phase 15)

### Erfolgskriterien

- LTP clock_gettime04 PASS (Perf-kritisch)
- Userspace-benchmarks zeigen <50ns/call

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

## Phase 17 — OOM-Killer + oom_score_adj

**Problem**: `pmm_alloc` bei ENOMEM gibt sofort -ENOMEM zurück, ohne zu
versuchen durch Killing von Prozessen Speicher freizugeben. cve-2017-17052
(und zukünftige Memory-Stress-Tests) bleiben nicht-deterministisch.

### Scope

- [ ] `task_struct.oom_score_adj` int16, -1000..1000, clone inherit
- [ ] `task_struct.oom_score_adj_min` (CAP_SYS_RESOURCE-Gate)
- [ ] `/proc/$pid/oom_score_adj` (rw)
- [ ] `/proc/$pid/oom_score` (ro, berechnet)
- [ ] `/proc/$pid/oom_adj` (legacy -17..15, scaled)
- [ ] `out_of_memory(alloc_ctx)` bei alloc_pages-fail:
  - score alle tasks: `(rss + swap + pgtables) / total * 1000 + oom_score_adj * 10`
  - highest-score: SIGKILL + async-reclaim
  - immune wenn adj == -1000
- [ ] init (pid 1) ist immune (panic statt kill)
- [ ] ktests: oom_score_adj clamp, inherit, OOM-victim-selection, init-protect

### Erfolgskriterien

- LTP cve-2017-17052 deterministisch PASS (nicht mehr flake)
- LTP oom01-05 (falls im install-Set): PASS
- Memory-Stress-Tests robust

### Abhängigkeit

Entkoppelt — kann nach Phase 13 jederzeit.

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
