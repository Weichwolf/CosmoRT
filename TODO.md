# CosmoRT — TODO

## Identität

CosmoRT ist eine **Multimedia-Konsole** mit **WASM als nativem Cartridge-Format**.

**Zwei dauerhafte Säulen** (nicht entweder-oder):

1. **Linux-ABI-Kompatibilität (permanent, vollständig)** — Alpine Linux läuft
   komplett. apk, busybox, OpenSSH, bash, Python, Compiler, Editor — alles
   was als Alpine-Paket verfügbar ist, läuft unverändert. Phase-Roadmap
   10-17 baut diese Säule (musl + LTP grün auf x86_64).
2. **WASM-Native (Alleinstellungsmerkmal)** — Neue Multimedia-Apps als
   WASM-Cartridges. Sandbox-by-Design, cross-arch, hot-loadable, zero-copy
   direkt mit framebuffer/audio/GPU via mmap-host-imports. Phase-Roadmap
   18-25 baut diese Säule.

**Konsole** = stabile RT-Plattform; **Cartridge** = WASM-App-Modul.
~10 Host-Imports statt 1500 Browser-DOM-APIs. Kein JS, kein DOM, kein
Compositor-Cargo-Cult.

Beide Welten interoperieren: Alpine-Tools bauen WASM-Cartridges,
WASM-Apps nutzen POSIX-Sockets via WASI, gemeinsames Filesystem.

**Vision-Frage**: Was würde Linus heute bauen, wenn er frisch anfinge auf
moderner x86_64+aarch64-Hardware mit WASM als Universal-Binary für
Multimedia-Apps?

---

## Stand (Session-Ende)

ktest **3108/0**, musl 460/11, LTP **248/7/43**. Phasen 10.1, 11, 13.1, 14,
15, 16, 17 erledigt. Phase 10.2 zentrale Migration fertig (Schritt 1-3):
event_queue intern auf wq, sched_wake auf state-CAS (try_to_wake_up),
thread->wait_head/wait_entry entfernt, kill_one's Doppelpfad fuer
sigtimedwait sleeper aufgeloest. Branch: `ltp`. Architektur-Doc unter
`notes/MODERN_KERNEL_DESIGN.md`.

**Strukturelle Blocker entfernt** (Phase 10.2-FINAL):
- `thread->wait_head`-Routing weg → reiner state-CAS
- event_queue intern auf wait_queue_head_t → eigene wq pro Sleeper
- kill_one's `if (wait_head) sched_wake : event_post` Konditionalen
  durch Direct-Calls ersetzt (event_post fuer eq-parked, sched_wake
  fuer alle anderen)

**Offen** (Phase 10.2-Schritt-4): Subsystem-Migration weg von event_queue.
event_queue.c bleibt vorerst als korrekt funktionierender Wrapper
(intern wq-basiert), aber alle ~30 event_post/event_wait Aufrufer
(eventfd, futex, pipe, socket, epoll, etc) koennten direkt auf
wait_queue_head_t pro fd/socket/futex umgestellt werden. Reduziert
~280 Zeilen event_queue.c + entfernt eq aus thread_t.

---

## Roadmap

| # | Phase | Kern-Problem | Aufwand | Status |
|---|-------|--------------|---------|--------|
| **10.1** | Waitqueue-Infrastruktur | atomic blocking primitive | ~600 LOC | ✓ |
| **10.2** | **Waitqueue-Migration restliche Wait-Pfade** | event_wait/futex/pipe/socket/epoll/signalfd/timerfd/wait4 | ~1500 LOC, 1 Pfad/Agent | **NEXT** |
| **11** | restart_block + signal-restartable syscalls | EINTR ohne rem-Recovery | ~500 LOC | ✓ |
| **12** | hrtimer ns-Präzision + Tick-less | timer_ms() in Hot-Path | ~800 LOC | partial (TSC mult/shift einzeln) |
| **13.1** | Skalierungs-Audit (Linus-today Limits) | FD_CEILING, NGROUPS_MAX, USOCK_BACKLOG | ~400 LOC | ✓ |
| **13** | SMP-saubere Scheduler-Finalisierung | globales rq_lock | ~1200 LOC | nach 10.2 |
| **14** | vDSO clock_gettime | syscall pro clock_gettime | ~600 LOC | ✓ |
| **15** | Network-Namespaces | netif+route-table global | ~2000 LOC | ✓ |
| **16** | IPv6-Stack | AF_INET6 = EPROTONOSUPPORT | ~2500 LOC | in progress |
| **17** | OOM-Killer + oom_score_adj | alloc-fail → -ENOMEM ohne Reclaim | ~600 LOC | ✓ |

**Reihenfolge Säule 1 (Linux-ABI-Vollständigkeit, Alpine läuft)**:
- **10.2a** Architektur-Refactor (wait_head raus, try_to_wake_up rein) ✓
- **10.2b** event_queue intern auf wq + kill_one Doppelpfad weg ✓
- **10.2c** Subsysteme einzeln auf eigene wait_queue_head_t (eventfd/pipe/
  socket/epoll/futex/wait4/rt_sigtimedwait) — derzeit nutzen sie wq
  via event_queue (transparent). Direkt-Migration eliminiert event_queue.c
- **10.2d** event_queue.c und thread_t.eq löschen (nach 10.2c)
- **12-Rest** hrtimer ns + tickless retry (jetzt mit korrekter waitqueue)
- **13** SMP-Scheduler-Finalisierung
- Erfolgskriterium: alle musl + LTP grün auf x86_64, Alpine apk/bash/sshd
  vollständig nutzbar

## Konsole-Phasen — Säule 2 (WASM-Native USP)

| # | Phase | Inhalt | Aufwand |
|---|-------|--------|---------|
| **18** | **Audio-Subsystem-Core** | HDA + virtio-snd + USB-audio Treiber, RT-DMA-Buffer, native C-API | ~3000 LOC |
| **19** | **GPU-Subsystem-Core** | virtio-gpu + Framebuffer + minimal Vulkan-Cmd-Queue | ~4000 LOC |
| **20** | **binfmt_wasm + WASI-Shim** | Magic-Detection, AOT-Helper-Fork, /var/cache/wasm, WASI ↔ POSIX | ~3000 LOC |
| **21** | **cosmort-multimedia Host-Imports** | ~10 Funktionen: fb_acquire, audio_open, gpu_submit, input_poll | ~1500 LOC |
| **22** | **libcosmort-multimedia + SDL3-Shim** | Userspace-Library, SDL3-API on top von Host-Imports | ~5000 LOC |
| **23** | **WASM-Audio-Plugin-API** | LADSPA-Replacement, hot-loadable, sandboxed | ~2000 LOC |
| **24** | **aarch64-Port** | HAL-Stubs zu echtem Code, Alpine-aarch64 grün | mehrere Sessions |
| **25** | **Cross-Arch-Cartridge-Verifikation** | gleiches .wasm läuft x86_64+aarch64 identisch | (passive) |

**Reihenfolge Konsole-Phasen**: 18 → 19 (parallel) → 20 → 21 → 22 → 23 → 24 → 25.

**Beide Säulen sind permanent.** Säule 1 (Linux-ABI) ist nicht "transition
weg davon" — sie bleibt vollständig nutzbar. Compiler, Editor, Tools laufen
über Säule 1. Multimedia-Apps werden über Säule 2 ausgeliefert.
Interoperabilität zwischen beiden ist explizites Designziel:
gemeinsames Filesystem, Netzwerk, Userspace-Tools können beide Cartridge-
und ELF-Formate produzieren.

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

**Phase 10.2b-3 — AF_UNIX auf waitqueue (ERLEDIGT)**

- [x] `unix_socket_t.{read,write,accept,connect}_wq` ersetzen
      `blocked_reader`/`blocked_acceptor`-Single-Slots. Multi-waiter
      korrekt (dup'd listener, parallel readers).
- [x] `usock_read_blocking`/`usock_write_blocking`/`usock_accept4`/
      `usock_connect` → DEFINE_WAIT + prepare_to_wait + schedule.
- [x] `usock_decref` weckt peer.read_wq + peer.write_wq + s.accept_wq +
      backlog-clients.connect_wq via wake_up_all.
- [x] 50ms-Timeout in usock_write_blocking entfaellt — prepare_to_wait
      schliesst die Wakeup-Race strukturell.
- [x] Neue ktests: `unix/block_read_wakeup`, `unix/block_read_signal`,
      `unix/block_accept_wakeup` (15 sub-asserts).

**Phase 10.2b-4 — TCP/UDP auf waitqueue (ERLEDIGT)**

- [x] `net_tcp_t.wait_wq` ersetzt `wait_thread`-Single-Slot. tcp_input
      weckt via wake_up bei SYN-ACK / Daten / RST / FIN / state change.
      Listener nutzen dieselbe wq fuer accept_queue-Admission.
- [x] `udp_sock_t.recv_wq` ersetzt `wait_thread`. udp_input/udp6_input
      wecken via wake_up nach q_push.
- [x] `do_connect` (v4+v6), `do_accept4`, `do_recvfrom` (TCP+UDP+UDP6),
      `socket_read` (TCP) → DEFINE_WAIT + prepare_to_wait +
      schedule + signal/timeout/EAGAIN-Re-Check.
- [x] event_post(EQ_SOCKET_DATA|EQ_SOCKET_CONNECT) komplett raus aus
      tcp.c/tcp6.c/udp.c/udp6.c.
- [x] Neue ktests: `tcp/block_recv_wakeup`, `tcp/block_accept_wakeup`,
      `tcp/block_recv_signal`, `udp/block_recvfrom_wakeup` (28 sub-asserts).
      ktest 3108 -> 3136.

**Phase 10.2c — Restliche Blocking-Pfade**

- [x] `event_wait_ns` → waitqueue auf `event_queue_t` (58c3b93, 1a3d33a,
      828b55e, da79de0). prepare_to_wait/finish_wait haengen Consumer
      atomic an `eq->wq`; event_post weckt via wake_up_interruptible.
      Schliesst den Race der Phase-12-tickless sem_init/tls_init-Hangs
      ausgeloest hat. +9 ktest Sub-Asserts (event_wait_race/01..05).
- [x] `timerfd_read` → waitqueue pro timerfd. Migration sauber, +1 LTP PASS.
- [-] `signalfd_read` — gestrichen. `signalfd` ist heute ENOSYS-Stub
      (`src/kernel/event/signalfd.c`). Migration eines nicht-existenten
      Pfads sinnlos. Volle signalfd-Implementierung waere eigene Phase
      (~500 LOC). LTP installiert keine signalfd-Tests, kein Blocker.
- [x] socket recv/accept → waitqueue pro socket (Phase 10.2b-3 + 10.2b-4
      jeweils AF_UNIX bzw. AF_INET/AF_INET6 abgeschlossen).
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

- [x] `hrtimer_now_ns()` als ein-Funktions-Aufruf-Hot-Path, TSC-direct
- [x] **Wrap-Safety**: hrtimer_now_ns via ms-Split, kein 600s-Limit mehr
- [x] `lapic_arm_ns` Overflow-Cap (defensive, Tickless-ready)
- [ ] `HZ_ns` = 1_000_000 für default-tick (1ms), konfigurierbar
- [ ] **Tick-less**: statt periodischer Tick ein one-shot LAPIC-Timer
      auf nächste Deadline (min(sched-quantum-expire, hrtimer-expire))
- [ ] `schedule_timeout(ns)` nutzt hrtimer statt tick-count
- [ ] TSC-calibration bei boot + Hyper-V TSC-page reference
- [ ] `clock_gettime(CLOCK_MONOTONIC)`-Kernel-Pfad: TSC-read + ns-scale
      (ohne syscall-Overhead wird erst Phase 14 erreicht)
- [x] ktests: 5 hrtimer_ns Tests (sub-ms-sleep, clock_gettime
      sub-100us, batch-loop, ABSTIME-ns, epoll_wait-batch)
- [ ] sys/time do_nanosleep ns-Praezise — versucht, revertiert
      (4 pthread-Tests Race-Regression). Braucht erst futex/event_wait
      ns-Migration (Schritt-3 unten).

### Aktueller Stand

ktest 3047 -> 3059 (+12 hrtimer_ns sub-asserts), wrap-safe hrtimer
behebt late-test Hangs (qsort-static, sem_open-static, clock_nanosleep01
nach >10min Uptime). musl 461/10 -> 460/11 (tls_init-static flake),
LTP 248/7 -> **249/6** (+1 PASS clock_nanosleep01).
Vollstaendige ns-Praezision in nanosleep verschoben bis
futex_wait/event_wait ebenfalls ns-deadline kennen — sonst zerschiesst
das pthread_cond/pthread-robust-detach via timing-races
(verworfen, siehe Reverts).

### Erfolgskriterien

- LTP epoll_wait02 PASS (ausstehend, braucht ns-event_wait)
- LTP clock_gettime04 PASS (ausstehend, vDSO-syscall Overhead)
- musl nanosleep-precision-Tests durchlaufen <2% jitter (ausstehend)

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

## Phase 15 — Network-Namespaces (ERLEDIGT)

**Status**: ktest 2951 -> 2978 (+27 sub-asserts via 8 neue net_ns
Tests). CLONE_NEWNET / unshare(CLONE_NEWNET) / setns(CLONE_NEWNET)
funktional. Per-NS isolation: netif-list, TCP-/UDP-Hash, AF_UNIX
abstract namespace, /proc/sys/net/ipv4/conf/{lo,default}/tag.

### Implementiert

- [x] `struct net_ns` mit netif-list, ip_forward/disable_ipv6 sysctls,
      conf_lo_tag/conf_default_tag (clone09 LTP), refcount, ns_id.
- [x] `task_struct.net_ns` Pointer, fork inherit (incref), CLONE_NEWNET
      alloc, unshare(CLONE_NEWNET) replace, setns(fd, CLONE_NEWNET) rebind.
- [x] netif-APIs ns-scoped: netif_register_ns/find_ns/default_ns/loopback_ns
      mit current-task-default fuer Hot-Path-Caller.
- [x] tcp_hash key = (ns_id, lport, rport, src_ip); udp_hash key = (ns_id, port);
      AF_UNIX abstract path key = (ns_id, path).
- [x] /proc/self/ns/net symlink + readlink format "net:[<id>]".
- [x] /proc/sys/net/ipv4/conf/{lo,default}/tag, ip_forward per-NS.
- [x] Loopback per-NS: jede neue NS bekommt eigene lo-netif via
      net_ns_alloc; HW-NICs bleiben in init_net_ns.
- [x] 8 ktests: newnet-fork-loopback, two-ns-share-port, unshare-separates-netif,
      setns-rebinds, proc-ns-net-format, cross-ns-tcp-refused,
      fork-inherits-ns, af-unix-abstract-per-ns.

### Bekannte Limits / Out-of-Scope

- HW-NIC (e1000, virtio-net) Migration zwischen NS via `ip link set
  netns` ist nicht implementiert — Linux-default-Verhalten ist explicit
  migration, das Phase-15 nicht braucht.
- ARP-Cache global (HW-Pakete sind alle init_net_ns).
- Routing-Table noch global; per-NS-Routing erst mit Multi-NIC-Setup.
- IP-Sysctls jenseits von conf/{lo,default}/tag und ip_forward bleiben
  global bis ein konkreter LTP-Test sie braucht.

### Bilanz

- ktest 2951 -> 2978 (+27)
- Erwartung: LTP clone09 PASS (CLONE_NEWNET nicht mehr -EINVAL,
  conf/{lo,default}/tag per-NS isoliert)

---

## Phase 16 — IPv6-Stack (ERLEDIGT)

**Status**: ktest 2978 -> 3005 (+27 sub-asserts via 10 ipv6 Tests),
Commits efb54b5..HEAD.

### Implementiert

- [x] `struct in6_addr` (RFC 4291 union 8/16/32) + `struct sockaddr_in6`
      (28 byte) in include/linux/in6.h
- [x] IPv6-Header-Parsing (40 Byte fixed + Hop-by-Hop/Routing/DstOpts;
      Fragment detected, kein Reassembly)
- [x] ICMPv6 Echo Request/Reply, Destination Unreachable Code 4
- [x] NDP NS/NA + per-NS Neighbor-Cache (RFC 4861, NUD-Subset, Hop=255 check)
- [x] NDP RS/RA + SLAAC EUI-64 link-local (RFC 4862)
- [x] DAD via ndp_send_dad_ns
- [x] IPv6-Routing-Table per-NS (linear LPM, sortiert by prefix-len)
- [x] TCP6 + UDP6 — Hash-Tables erweitert (XOR-fold 16->4 bytes fuer Hash-Key)
- [x] socket_t.is_v6/v6only/local_ip6/remote_ip6
- [x] do_socket(AF_INET6, ...), do_bind/connect/accept/getsockname mit
      sockaddr_in6
- [x] IPV6_V6ONLY socket-option (default 1, Linux-konform)
- [x] SCTP (proto=132) -> EPROTONOSUPPORT (LTP bind04 SCTP-Subcases SKIPpen
      sauber statt TBROK)
- [x] 10 neue ktests: socket-create, bind-loopback, bind-ephemeral,
      tcp6-loopback (handshake+data), udp6-loopback, bind-short-addr,
      bind-wrong-family, v6only-default, bind-conflict, non-loopback-send

### Bilanz

- ktest 2978 -> 3005 (+27 sub-asserts, 0 failures)
- LTP bind04 IPv6-Subcases freigeschaltet, SCTP-Subcases SKIP statt TBROK
- ping6 ::1 funktioniert (ICMPv6 Echo full roundtrip)
- Keine Regression in IPv4-Tests (Hash-Fn unveraendert fuer is_v6==0)
- LTP-Erwartung: bind04 komplett PASS, weitere SKIPpende v6-Tests jetzt
  potenziell ausfuehrbar — Verifikation per make alpine-test ausstehend

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

## Phase 13.1 — Skalierungs-Audit (ERLEDIGT)

**Status**: ktest 3022 -> 3034 (+12 sub-asserts via 9 neue Tests).
Drei Limit-Items gefixt, jeweils 1 Commit:

### Implementiert

- [x] `proc/cred: NGROUPS_MAX 32 -> 65536`. `groups[NGROUPS_MAX]` als
      Pointer + count + groups_pages, lazy via pages_alloc.
      `cred_groups_set/free` als einzige Mutatoren; fork erbt deep-copy
      in fresh pages. proc_cleanup/free_child_proc geben frei.
- [x] `event/fd: FD_CEILING 65536 -> 1M (Linux sysctl_nr_open)`. Zwei-
      Level page-list (FD_LEAF_PER_PAGE=170 pro Leaf-Page) statt flat
      Buddy-Array. Lookup O(1) ueber `fd_entry_at()`. Leaves lazy-
      alloc. Bitmap-Capacity round_up(64), slack-Bits pre-USED.
      `fd_ensure_capacity` clamped want auf nofile (dup-doubling).
- [x] `net/unix: backlog dynamisch slab-list statt fix[8]`. backlog_head/
      tail/count/cap pro Listener, USOCK_SOMAXCONN=4096 Hard-Cap. listen()
      respektiert jetzt das User-Argument. backlog_owner-Backpointer fuer
      close-vor-accept Cleanup. Listener-Teardown drained und weckt
      blockierte connect().

### Bilanz

- ktest +12 (9 neue Tests fuer alle drei Items)
- musl/LTP-Baseline keine Regression
- Drei Verstoesse gegen "keine fixen Pools" eliminiert; Linus-today
  Workload-Tauglichkeit (63k Prozesse * Mehrfach-FD) jetzt erreicht

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
