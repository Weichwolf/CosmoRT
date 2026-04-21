# CosmoRT — TODO

Stand: ktest **2526/0** (Varianz 0, bekannter tcp-transfer-Flake 1/5), musl 462/10, LTP Alpine-Baseline im Fluss. Branch: `ltp`.

## Offene Phasen

| # | Phase | Status | Aufwand | Blocker für |
|---|-------|--------|---------|-------------|
| 7.3 Rest | abgeschlossen | — | — | — |
| **7.7** | **Timer-Treiber (clocksource + ACPI + virtio + Hyper-V)** | neu | ~11 Tage | RT-Audio-Präzision |
| 8.1 | Audio | neu | langer Neubau | RT-Audio-Identität |
| musl/LTP Fails | nach Timer | — | — | — |
| 9 | aarch64-Port | neu | mehrere Sessions | Multi-Arch |

---

## Phase 7.3 — Offene Pools

18 migriert, 0 offen. Einer pro Task-Session.

**Erledigt (diese Session):**
- [x] `EXT4_OPEN_MAX=256` (`fs/vfs.c`) → chained Hash-Table. 64 Buckets (power of 2, Mask statt Modulo), Knuth-Multiplikator auf `ino`. Entries slab-dynamic (`ext4_open_slab`, initial 64). `ext4_open_inc` alloziert außerhalb des Locks mit Recheck nach Acquire (Race-Safe). Lookup/Insert/Remove O(1) average. `_Static_assert` auf power-of-2 Hash-Size. Linearer 256-Slot-Scan unter `ext4_open_lock` eliminiert.
- [x] `EQ_MAX_EVENTS=16` → Ring-Wachstum bei Overflow. Initial 256 Events (1 Page), verdoppelt bei Bedarf via `pages_alloc` unter `eq_lock`. Events lossless (kein Overwrite mehr), bei OOM Fallback auf alten Overwrite-Pfad. `event_queue_init/destroy` aus `thread_alloc/free` + exec-Reset via `event_queue_reset`. Keine Header-Inline-Änderung am Fast-Path (eq_pop/eq_push weiter inline, Feld-basiert statt Makro-Mask).
- [x] `PTY_MAX=12` → dynamischer Slab + linked list. `pty_alloc()` liefert auto-increment ID, `pty_get(int id)` macht Linear-Search durch Liste. `/dev/pts/N`-Namespace via neue Konstante `PTY_DEV_ID_MAX=256` begrenzt (nicht Pool-Grenze, nur valide ID-Range).
- [x] `EQ_LOCK_MAX=512` → `spinlock_t eq_lock` in `thread_t` (ans Ende der Struct, keine ABI-Offset-Shifts). TID-Hash-Array entfernt. Lock lebt mit dem Thread, Init via `kmemset` in `thread_alloc`. thread_t-Größe unverändert 3136 (Padding absorbiert).
- [x] `PID_TABLE_MAX=4096` + `TID_TABLE_MAX=4096` → dynamisches Pointer-Array. Initial 256 Slots (1 Page), Verdopplung via `pages_alloc` unter `pid_lock` wenn Tabelle voll ist. Obergrenze `PID_MAX_CEILING = 1<<22` (Linux-kompatibel, `/proc/sys/kernel/pid_max` Default). `pid_table_capacity()` / `tid_table_capacity()` als Iterations-API für Caller statt ehemaliger Compile-Zeit-Konstante. `alloc_next_id()` kapselt monotoner Wrap + Grow-on-Miss; Hot-Path `proc_find`/`thread_find_by_tid` bleibt O(1).
- [x] `EXECVE_MAX_*` → Linux-konform. `EXECVE_MAX_ARGS/ENVS` 256→4096, `EXECVE_MAX_STRLEN` 4K→128K (Linux MAX_ARG_STRLEN = 32*PAGE_SIZE). Alle Pointer-Arrays (`kargv_ptrs/kenvp_ptrs/new_argv` in `do_execve`, `argv_addrs/envp_addrs` in `build_user_stack`) via `pages_alloc` heap-alloziert — bei 4096 Slots wäre jedes 32KB und würde den 16KB-Kernel-Stack sprengen. Silent-Truncation eliminiert: Overflow (argc ≥ MAX, buf voll, String > MAX_STRLEN) gibt strict `-E2BIG`; Alloc-Fehler gibt `-ENOMEM`; user-pointer-Failure gibt `-EFAULT`. Cleanup auf allen 4 Fehlerpfaden + Erfolgspfad. `_Static_assert` auf Pool-Größen + buddy-Cap. EXECVE_BUF_SIZE (128KB) unverändert = Alpine ARG_MAX = total envelope argv+envp.

**Erledigt (Forts.):**
- [x] `RLIMIT_NPROC`, `RLIMIT_FSIZE`, `RLIMIT_CPU` verdrahtet. NPROC: Check in `kernel_clone` via `proc_count_alive()`, `-EAGAIN` bei Überschreitung. FSIZE: Check in `vfs_write`/`vfs_pwrite`/`vfs_truncate`/`vfs_ftruncate`; Partial-Write klemmt `count` auf `limit - offset`, bei `offset >= limit` -EFBIG + SIGXFSZ. O_APPEND: effektiver Offset = aktuelle Dateigröße. CPU: Tick-Callback bei 1000Hz, akkumuliert `p->cpu_time_ticks`; bei Soft-Limit SIGXCPU (1×/sec), Hard-Limit SIGKILL — beides via `sig_pending`-Bit (delivery auf Syscall-Return, nicht in IRQ-Kontext). Fork erbt jetzt auch `rlim_nofile` (war latent nicht kopiert). Defaults: alle `RLIM_INFINITY` (0 = unlimited, Linux-kompatibel).

**Erledigt (Forts.):**
- [x] FD-Tabelle `FD_MAX=1024` → dynamische Expansion (Linux `expand_fdtable`). Initial 64 Slots (`FD_INIT_SLOTS` = BITS_PER_LONG), verdoppelt on-demand in `fd_alloc`/`fd_dup_at`/`fd_install_at` bis `FD_CEILING=65536` (2MB buddy-Cap für `fd_entry_t`-Array). Default `rlim_nofile=0` bleibt „unset" → `FD_DEFAULT_NOFILE=1024` (Linux ulimit -n Default). `entries[]` + `free_bitmap[]` jetzt Pointer, page-alloziert. `fd_table_init`/`fd_table_free`/`fd_table_alloc_empty`-API. Fork: `dup_fd_table` respektiert child's `rlim_nofile` (overshoot → dropped). dup/dup2/dup3/F_DUPFD: `fd_entry_t`-Kopie auf Stack vor potentieller Re-Alloc (Pointer-Stability). setrlimit(NOFILE, 4096) + 2000 dups getestet.

**Offen:** —

---

## Phase 7.7 — Timer-Treiber (Clocksource-Abstraktion + Hardware-Treiber)

Research: `notes/TIMER_DRIVERS.md` (Commit `e4ff691`). Kernbefund: Hyper-V TSC-Page gemappt aber ungenutzt, keine `struct clocksource`-Abstraktion, kein ACPI-Parser. Motivation: RT-Audio-Präzision + Hypervisor-bounded Latenz.

Priorität nach Aufwand × ROI (bindende Reihenfolge):

- [x] **7.7.1** `struct clocksource`/`clock_event_device` Core (`src/kernel/core/clocksource.{c,h}`). TSC als einzige clocksource rating 300, `hal_timer_now_ns` → `clocksource_read_ns()`. 2505/0.
- [x] **7.7.2** ACPI Table-Parser (`src/kernel/hw/acpi.{c,h}`). RSDP aus `boot_info.rsdp_addr` → XSDT/RSDT-Walk → `acpi_find_table()`. FADT/HPET/MADT-Layout + MADT-Counts. 11 neue Tests (subcases 20-30). 2526/0.
- [ ] **7.7.3** HPET-Treiber (`src/arch/x86_64/timer/hpet.c`) — clocksource rating 250 + clock_event pro Comparator. Eliminiert 10ms PIT-Kalibration. ~300 LOC, 1.5d.
- [ ] **7.7.4** Hyper-V TSC-Page als clocksource (`src/arch/x86_64/hw/hyperv_clocksource.c`) — trivialer Wrapper um bestehenden `hyperv_tsc_time_ns`, rating 400. Fix für latenten Bug unter Hyper-V/WSL2. ~60 LOC, 0.3d.
- [ ] **7.7.5** KVM pvclock (`src/arch/x86_64/hw/kvmclock.c`) — `wall_clock` + `system_time` MSRs, TSC-Scale vom Host. rating 400. Läuft unter `make qemu-*`. ~200 LOC, 1d.
- [ ] **7.7.6** Hyper-V STimer (`src/arch/x86_64/hw/hyperv_stimer.c`) — 4 synthetic timer pro vCPU, SynIC-Interrupts, hypercall-basiert. clock_event rating 400. ~400 LOC, 2d.
- [ ] **7.7.7** ACPI PM_TMR (`src/arch/x86_64/timer/acpi_pm.c`) — Fallback-clocksource rating 110, 3.579545 MHz. ~120 LOC, 0.5d.
- [ ] **7.7.8** virtio-rtc (`src/drivers/virtio/virtio_rtc.c`) — optional, für Guest-Umgebungen. ~300 LOC, 1.5d.
- [ ] **7.7.9** TSC-Invariant-Check (`CPUID.80000007H:EDX[8]`) + Boot-Zeit-Kalibrierung via HPET statt PIT.

---

## Phase 8.1 — Audio (Kern-Identität)

CosmoRT ist Audio-Realtime-Kernel. `notes/NOTES.md` sagt 0%. Reine Neuentwicklung.

- [ ] `drivers/audio/hda.c` Intel HDA-Treiber
- [ ] `drivers/audio/virtio_snd.c` QEMU virtio-sound
- [ ] `drivers/audio/audio.c` Ring-Buffer, `/dev/snd/*`
- [ ] ALSA-ABI oder eigene minimal-API (`notes/AUDIO.md` entscheiden)
- [ ] RT-Scheduling-Pfad bis Audio-Thread bounded

---

## Phase 9 — aarch64-Port

HAL-Stubs bereits in `src/arch/aarch64/hal_*.c` (Phase 7.2). Interface-Oberfläche aarch64-generic.

- [ ] `boot/entry.S` EL2→EL1 Drop, Stack-Init
- [ ] `cpu/exceptions.c` ESR-Decode, Vector-Table
- [ ] `mm/paging.c` 4K Pages, 3-Level PT, TLBI
- [ ] `irq/gic.c` GICv2/v3
- [ ] `timer/arch_timer.c` Generic Timer CNTV_CTL
- [ ] `smp/smp.c` PSCI CPU_ON (statt Trampoline <1MB)
- [ ] `syscall/entry.S` SVC #0 Pfad
- [ ] `context.S` x0-x30 + FPSIMD
- [ ] Boot-Test: QEMU aarch64 virt-Machine

**Blockiert Phase 7.4**: Lock-Granularität-Migration erst wenn SMP_MAX_CORES > 1.

**Folge-Audit bei Aktivierung:** `core/irq.c` APIC-Init → `src/arch/x86_64/irq/` verschieben, `hal_smp_boot_ap` generalisieren.

---

## Restaufgaben

- [ ] `ltp/copy_file_range-basic` "data matches": 31 Bytes kopiert, Inhalt diff. `copy_file_range-offsets`+`-03` grün. Ohne Trace nicht lokal.
- [ ] `timer_wheel_tick` wird im Kernel nicht aufgerufen (Pool-12-Audit-Befund) — dead code, vermutlich seit RT-Scheduler-Rewrite entkoppelt. Callsite prüfen und reaktivieren oder API entsorgen.

---

## Non-Kernel

### musl libc-test (10 FAIL)

- [ ] `malloc-brk-fail` (1): t_vmfill schafft 4GB-Exhaustion nicht.
- [ ] `fma`/`fmal`/`powf`/`remquol` (4): QEMU-softfloat Abweichung, nicht Kernel.
- [ ] `tls_get_new-dtv` (1): dlopen-Szenario, musl-Userspace-Segfault.
- [ ] `pthread_atfork-errno-clobber` (2): errno-Pfad im fork() mit mehreren Threads.
- [ ] `rlimit-open-files` (2): setrlimit(RLIMIT_NOFILE) + FD-Verhalten.

### LTP Alpine

- [ ] Volle Baseline-Messung. Alter "87 FAIL"-Wert veraltet. Run hängt deterministisch nach `fgetwc-buffering-static` (vermutlich dynamischer Pendant `fgetwc-buffering` — Dynamic-Loader/Shared-Memory-Bug? Skip + reproduzieren).
- [ ] `VMA/TLB Race`: Atomarer VMA-Update + TLB-Shootdown (mit Phase 6.3 kompatibel).

---

## Archiv (erledigte Phasen + Commits)

| Phase | Ergebnis | Schlüssel-Commits |
|-------|----------|-------------------|
| 0 Build-Infra | Header-Deps `-MMD -MP`, stale-`.o`-Bug erschlagen | `5d17930` |
| 0.5 Test-Runner-Watchdog | alarm→parent-SIGALRM+wait4, Varianz ±14→±2 | `ea88825` |
| 1 Syscall-Validierung | 2334/79 → 2349/65 (+15) | `c9c2d9b`, `4501cf7`, `2c26795` |
| 2 VFS-Metadaten | fill_stat, SUID/SGID-clearing, creat mode-0 | `560157d` |
| Signal/Scheduler-Bugs | 3× STOPPED/RUNNABLE-State-Bugs | `18dc8a2` |
| 3 sched_preempt-Refactor | 5 Frame-Sync-Stellen → 1 Helper, 110 Magic-Indices → 0 | `1fe111b`, `78c8ed5`, `967a550` |
| 4 Stub-Implementierungen | flock, caps, execveat, eventfd-sem, xattr, chroot, acct, fadvise64 | `01fa5e8`, `90cb4c2`, `bbbb68b`, `91fb558`, `8a7f8e6`, `8b9d8fa`, `9a0a37a`, `0a96060`, `e5f8d3a` |
| 5 Loopback | connect EISCONN/ECONNREFUSED, accept-Deadline-Entfernung, ARP NUD-States + Pending-Queue + 2 latente UAFs | `9543eae`, `9606617`, `b10a495`, `0188169` |
| 6.1 Signal-Delivery | Verifiziert (durch frühere Phasen bereits korrekt), alarm07b reaktiviert | `7c49be6` |
| 6.2 Timing | Bereits korrekt; Kernel-Bug gefixt (`smp_core_count` NULL-Jump in /proc/stat), clone301-Test-Port-Bug | `d888bcb`, `ff4c1ab`, `1dcff71` |
| 6.3 Fork/CoW | CoW war bereits korrekt, Test-Bug gefixt (stack-local→BSS) | `c15d83a` |
| 6.4 Stack-Guard-Page | PROT_NONE-VMA am Wachstumslimit | `c40eab3` |
| 6.5 Socket-Wakeup | do_poll in epoll-sleeper-Liste, ARP Signal-Check, TCP-Hash-Korruption im Accept-Pfad gefixt | `0dda997`, `0fc6741` |
| 6.6 Page-Fault-Recovery | extable + PROT_NONE-Kernel-Guard (echte Wurzel der 140-Test-Regression) | `2eaad4f`, `57a2952`, `6421623`, `7e53d07`, `88b6e3a`, `5d3ba99`, `0741ca8` |
| 7.1 Tick-Callback-Registry | 6 Subsysteme via Registry, sched_preempt 125→93 Zeilen | `7cc77de`, `7277835`, `e53b8f8`, `4ccad5f` |
| 7.2 HAL durchsetzen + 7.6 Layer | `arch_*` 147→0, `asm volatile` 9→0, aarch64-Stubs bereit | `147ba3b`, `1ed1211`, `530b8c3`, `3c19924`, `70b391b` |
| 7.4 Lock-Granularität | Audit-only, deferred bis SMP-N (Migrationsplan in Archiv) | `c8b35f5` |
| 7.5 RCU-Vollendung | Tick-basierte Deferred-Execution, Range-Check-Helper | `bc8dd55`, `19b3310` |
| 7.3 Pool-Migration (10) | FD/PIPE/NET_TCP/NET_SOCK/USOCK/ACCEPT_QUEUE/VMA/MOUNT/ARP/HW_MAX_HANDLERS | `01fa5e8`..`d96c95f`, plus TCP-OOO `64372a0`, UDP `b185178`, TW `fefa4ca` |
| 8.3 procfs | mmap-fileop, /proc/stat, /proc/pid/status/stat vervollständigt | `0e89143`, `b16c3c3` |
| Einzelbug-Sweep | adjtimex03, faccessat202, EPOLL-CLOEXEC, close_range-cloexec, EPOLLONESHOT, epoll_ctl02-eperm, execve03, acct(".") | `0a96060`..`f6b79d1` |
| Build-Härte | `-z defs`, `-Wl,--no-undefined`, alpine-test Timeout 600s→1800s, boot-test.sh robust | `6d10e1c` |
| musl Regressionen | sem_open/pthread_robust/tmpfs-Mount-abspath-Bug, fd_cleanup_entry Pipe-Refcount-Bug aus 7.3, ext4-Extent-Skip, `vfs_futimensat_ext4` NULL-Jump | `d630392`, `9e3a1f1`, `e31ff40` |

---

## Session-Bilanz

| Metrik | Start | Jetzt |
|---|---|---|
| ktest passed | 2334 | **2429** (+95) |
| ktest failed | 79 | **0** |
| Varianz | ±14 | 0 |
| musl passed | 446 | 462 (+16) |
| Kernel-Bugs gefixt | — | ~12 (Signal/Sched/TCP-Hash/PROT_NONE/slab-grow/pipe-refcount/undef-symbol ×2/UAF ×2) |
