# CosmoRT — TODO

Stand: ktest **2427/0** (Varianz 0), musl 462/10, LTP Alpine-Baseline im Fluss. Branch: `ltp`.

## Offene Phasen

| # | Phase | Status | Aufwand | Blocker für |
|---|-------|--------|---------|-------------|
| 7.3 Rest | 6 Pools offen | 2 in Arbeit | 1 Tag | — |
| 8.1 | Audio | neu | langer Neubau | RT-Audio-Identität |
| 9 | aarch64-Port | neu | mehrere Sessions | Multi-Arch |

---

## Phase 7.3 — Offene Pools

11 migriert, 1 in Arbeit, 6 offen. Einer pro Task-Session.

**Erledigt (diese Session):**
- [x] `EQ_MAX_EVENTS=16` → Ring-Wachstum bei Overflow. Initial 256 Events (1 Page), verdoppelt bei Bedarf via `pages_alloc` unter `eq_lock`. Events lossless (kein Overwrite mehr), bei OOM Fallback auf alten Overwrite-Pfad. `event_queue_init/destroy` aus `thread_alloc/free` + exec-Reset via `event_queue_reset`. Keine Header-Inline-Änderung am Fast-Path (eq_pop/eq_push weiter inline, Feld-basiert statt Makro-Mask).

**Offen:**
- [ ] `PID_TABLE_MAX=4096` → Radix-Tree/IDR (aufwändig; 4096 für Single-User praktisch genug).
- [ ] `TID_TABLE_MAX=4096` → wie PID.
- [ ] `PTY_MAX=12` → Slab (Signatur-Änderung: `pty_get(int id)` vs. Pointer-Rückgabe).
- [ ] `EQ_LOCK_MAX=512` → struktureller Umbau, per-thread Lock.
- [ ] `EXECVE_MAX_*` → 128KB-Buffer ist laut Audit bereits Linux-kompatibel; Verify-only.
- [ ] `EXT4_OPEN_MAX=256` (`fs/vfs.c:283`) → lineare Suche unter Lock. Hash-Table. Aus 7.4-Audit abgeleitet.
- [ ] `_Static_assert` auf kritische Slab-Struct-Größen.
- [ ] `RLIMIT_NPROC`, `RLIMIT_FSIZE`, `RLIMIT_CPU` — noch nicht verdrahtet.
- [ ] FD-Tabelle echte dynamische Expansion (Linux 32→64→∞). Heute fix 1024 mit RLIMIT_NOFILE, ausreichend für Single-User.

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
| ktest passed | 2334 | **2427** (+93) |
| ktest failed | 79 | **0** |
| Varianz | ±14 | 0 |
| musl passed | 446 | 462 (+16) |
| Kernel-Bugs gefixt | — | ~12 (Signal/Sched/TCP-Hash/PROT_NONE/slab-grow/pipe-refcount/undef-symbol ×2/UAF ×2) |
