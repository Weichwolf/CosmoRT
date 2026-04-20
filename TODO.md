# CosmoRT — TODO

Stand: ktest **2427/0** (Phase 8.3 abgeschlossen, Varianz 0), musl 452/20, LTP 11/87. Branch: `ltp`.

Priorisierung aus Architektur-Audit. Reihenfolge ist bindend: spätere Phasen setzen frühere voraus.

## Phasen-Übersicht

| # | Phase | Fails adressiert | Aufwand | Risiko | Blocker für |
|---|-------|------------------|---------|--------|-------------|
| 0 | Build-Infrastruktur (Header-Deps) | **✓ done** (Commit `5d17930`, 2335/79 → 2341/78) | — | — | — |
| 1 | Syscall-Validierung (Klasse D) | **✓ done** (2334/79 → 2349/65) | — | — | — |
| 2 | VFS-Metadaten (Klasse B) | **✓ done** (2354/65 → 2361/58, Commit `560157d`) | — | — | — |
| 0.5 | Test-Runner-Watchdog | **✓ done** (2361/58 → 2364/56, Varianz ±14 → ±2) | — | — | — |
| 3 | `sched_preempt`-Refactor | 0 direkt | 3 Tage | mittel | Phase 6 |
| 4 | Stub-Implementierungen (Klasse A) | **✓ done** (2365/55 → 2400/19, +35; 4.8 nach Phase 6.6) | — | — | — |
| 5 | Loopback-Vollendung (Klasse C) | **✓ done** (2403/16 → 2405/14) | — | — | — |
| 6 | Race/Signal-Pfad (Klasse E) + **6.5 Socket-Wakeup ✓** + **6.6 Page-Fault-Recovery (extable) ✓** | ~10 + 4 Netz ✓ + 4 acct ✓ | 4+3+2 Tage | **hoch** | 4.8 (acct) ✓ |
| 7 | Architektur-Schulden | 0 direkt | kontinuierlich | mittel | — |
| 8 | Fehlende Subsysteme (Audio, Caps, Guard-Page) | qualitativ | lang | niedrig | — |

**Phase 0.5** nach Netz-Instabilitäts-Audit eingefügt (Diagnose-Run nach Phase 2): Test-Count schwankt 2349..2361 über 5 Runs. Wurzel zweigeteilt — Runner-`alarm(5)` im Child kollidiert mit Kernel-Busy-Waits (ARP/DHCP bis 3s), und `poll()` hat keinen Readiness-Wakeup aus dem Netz-Stack (→ Phase 6.5).

---

## Phase 0 — Build-Infrastruktur ✓ abgeschlossen

Commit `5d17930`. Test-Stand direkt nach sauberem Rebuild von 2335/79 auf 2341/78 gestiegen — stale-`.o`-Bug war real, 6 Tests fälschlich rot.

- [x] `-MMD -MP` in KCFLAGS/EFI_CFLAGS/DRVFLAGS/ASFLAGS + test TCFLAGS/KCFLAGS_NO_BUILD
- [x] `.d`-Files neben `.o` (kein separates `build/deps/`-Layout — unnötige Indirektion)
- [x] `-include $(ALL_OBJ:.o=.d)` am Makefile-Ende (root + test)
- [x] Validierung: `touch thread.h` → 48 Kernel-`.o` rebuild, `touch ktest.h` → 163/163 Test-`.o`; idempotent
- [x] `make clean` löscht `$(BUILD)` komplett (inkl. `.d`)

---

## Phase 1 — Syscall-Input-Validierung

Low-risk, mechanisch. Jede Box ≤ 10 Zeilen Code.

### 1.1 Flag-/Argument-Checks

- [x] `close_range`: unknown flags → `-EINVAL`
- [x] `dup3`: `flags & ~O_CLOEXEC` → `-EINVAL`, O_CLOEXEC anwenden
- [x] `fchmodat2`: unknown flags → `-EINVAL`
- [x] `fchownat`: unknown flags → `-EINVAL`
- [x] `fdatasync` auf pipe/socket → `-EINVAL`
- [x] `epoll_create`: `size <= 0` → `-EINVAL`
- [x] `epoll_create1`: unknown flags → `-EINVAL`
- [x] `epoll_ctl`: `fd == epfd` → `-EINVAL`
- [x] `epoll_wait` auf Non-Epoll-FD → `-EINVAL` statt `-EBADF`
- [x] `epoll_pwait2`: `tv_sec<0 | tv_nsec<0 | tv_nsec>=1e9` → `-EINVAL`
- [x] `clone`: `CLONE_VM` ohne user-stack → `-EINVAL`
- [x] `clone`: `CLONE_SIGHAND` ohne `CLONE_VM` → `-EINVAL`
- [x] `clone`: `CLONE_THREAD` ohne `CLONE_SIGHAND` → `-EINVAL`
- [x] `clone3`: `size < sizeof(clone_args_min)` → `-EINVAL` (CLONE_ARGS_SIZE_VER0=64)
- [x] `clock_settime`: `CLOCK_MONOTONIC/BOOTTIME` → `-EINVAL`
- [x] `clock_settime`: `clock_id` Range-Check
- [x] `clock_settime`/`clock_nanosleep`: `timespec` (`tv_sec<0`, `tv_nsec<0|>=1e9`) validieren
- [x] `clock_settime`: NULL `timespec` → `-EFAULT`
- [x] `adjtimex`: NULL `timex` → `-EFAULT`
- [x] `adjtimex`: `mode` Flag-Validierung
- [x] `adjtimex`: `tick` Range (900_000..1_100_000)
- [x] `bind` doppelt → `-EINVAL`
- [ ] `chmod`/`chown` leerer Pfad → `-ENOENT` (Phase 2, im VFS-Lookup)
- [x] `eventfd` write: `val == UINT64_MAX` → `-EINVAL` (per man eventfd)
- [x] `fallocate`: `mode & ~(KEEP_SIZE|PUNCH_HOLE|...)` → `-EOPNOTSUPP`
- [x] `read` auf O_WRONLY-fd → `-EBADF`
- [x] `write` auf O_RDONLY-fd → `-EBADF`

### 1.2 FD-Typ-Dispatch (`sock_from_fd`)

Neuer `sock_lookup(fd, &err)`: EBADF bei Non-FD, ENOTSOCK bei Non-Socket.
Umgebaut: sendto, recvfrom, setsockopt, getsockopt, getsockname,
getpeername, shutdown.

- [x] `sock_from_fd` umbauen (→ `sock_lookup`)
- [x] `accept` auf `SOCK_DGRAM` → `-EOPNOTSUPP` (bereits vorher vorhanden)

### 1.3 Permissions

- [x] `access(X_OK)`: `i_mode & (S_IXUSR|S_IXGRP|S_IXOTH)` prüfen

---

## Phase 2 — VFS-Metadaten ✓ abgeschlossen

Commit `560157d`. Test-Stand 2354/65 → 2361/58. POSIX-Inode-Felder (uid/gid/mode/times/nlink) durchgereicht, fill_stat als Single Source of Truth, SUID/SGID-Clearing nach Linux-Spec, creat mode-0 honoriert, Parent-Dir-Check vor O_CREAT.

### 2.1 `vfs_node`-Erweiterung

- [x] Felder: `uid`, `gid`, `mode` (07777+type separat), `atime/mtime/ctime` (uint64_t seconds, 64-bit durchgehend), `nlink`
- [x] Slab-allokiert (`VFS_INODE_MAX=256`, existierender Pool)

### 2.2 Syscall-Durchreichung

- [x] `do_chmod`/`do_fchmod`/`do_fchmodat`: `i_mode = new_mode & 07777` + ctime-update
- [x] `do_chown`/`do_fchown`/`do_fchownat`: `i_uid/i_gid` setzen, SUID/SGID nach Linux `fs/attr.c` (nicht bei DIR; S_ISGID nur wenn S_IXGRP gesetzt)
- [x] `do_creat`: mode 0 respektieren (war: ignoriert → 0644)
- [x] `do_stat`/`do_fstat`/`do_lstat`/`do_fstatat`: `fill_stat` liest uid/gid/mode/times direkt aus vnode
- [x] `fill_stat`: Single Source of Truth; Symlink-Size bei symlink-Create gesetzt, redundante Branches entfernt
- [x] Hard-Link (`link`, `linkat`): `i_nlink++` in tmpfs (ext4: `ip.i_links_count++` war bereits da)
- [x] `atime`/`mtime` 64-bit: `inode->atime/mtime` ist `uint64_t`, direkter Pass-Through nach `st_atime_sec` (`int64_t`). Test `fs/utime-64bit` mit `tv_sec = 1LL<<32` grün.

---

## Phase 0.5 — Test-Runner-Watchdog ✓ abgeschlossen

Parent-seitiger Watchdog via SIGALRM-Handler + blocking `wait4`. Handler setzt Flag und `kill(-pgid, SIGKILL)`. Child setzt `setsid()` für Process-Group-Isolation. `alarm()` im Child entfernt — LTP-alarm02..07 nicht mehr blockiert.

Varianz 5×: Pre-Fix 2348..2362 (±14), Post-Fix 2363..2365 (±2). Reststreuung: `net/connect EISCONN` (4/5) aus Phase 6.5, `SIGCONT: WIFCONTINUED` (1/5) — echter Kernel-Race SIGSTOP→SIGCONT→SIGKILL auf Grandchild, vorher von alarm(5) maskiert. Null-Varianz erfordert Phase 6.5 + Kernel-Signal-Fix, beides out-of-scope.

Poll-Loop mit `nanosleep(10ms)` erprobt, verworfen: nanosleep hängt deterministisch nach bestimmten SIGSTOP/SIGCONT-Sequenzen (`test_job_control`). Blocking-wait4-Pfad umgeht den defekten Codepfad. Root-Cause im Scheduler/Event-Queue, nicht im Runner.

- [x] Parent-seitiger SIGALRM-Watchdog mit `kill(-pgid, SIGKILL)` on timeout
- [x] `alarm()` im Child entfernt
- [x] Timeout pro Kategorie: net=10s, crash/fuzz=15s, default=5s (unit=2s zu eng für fork+sleep-Tests wie `sig-bug3-per-thread-mask`)
- [x] Löst die 5 LTP-alarm-Tests (alarm02/03/05/06/07)
- [~] Test-Count-Varianz: ±2 statt ±14 (Rest: 2 Tests, beide Kernel-Bugs nicht Runner)

---

## Phase 3 — `sched_preempt`-Refactor ✓ Struktur-Sanierung abgeschlossen

**Begründung:** Blockiert Diagnose der Phase-6-Races. `sched_preempt` hatte zwei Frame-Save-Pfade, Frame-Sync war in fünf Stellen dupliziert (sched.c ×2, irq.c ×3 für INT 0x80 / SIGSEGV / Exception). Magic-Indices `f[18] & 3`. FS_BASE asymmetrisch. XSAVE implizit safe nur wegen `-mgeneral-regs-only`.

Commits `1fe111b`, `78c8ed5`, `967a550`. 5× dup Frame-Sync → 1× in `include/kernel/core/frame.h`. Zero `f[N]`-Matches in sched.c+irq.c. 2416/3 stabil, Varianz 0.

### 3.1 Frame-Sync-Primitive ✓

- [x] `irq_frame_to_thread`, `thread_to_irq_frame`, `frame_is_user` als static-inline in `include/kernel/core/frame.h`
- [x] `_Static_assert` auf `irq_frame_t`-Layout (sizeof + alle genutzten Offsets)
- [x] Typed `irq_frame_t*` durch `sched_preempt` statt `void*`
- [x] Magic-Index `f[18] & 3` → `frame_is_user(f)`

### 3.2 Duplikate eliminieren ✓

- [x] `sched.c` Signal-Delivery (237-251) → Helper
- [x] `sched.c` Reschedule-Save (270-290) → Helper
- [x] `irq.c:170-196` (INT 0x80) → Helper
- [x] `irq.c:573-596` (SIGSEGV) → Helper
- [x] `irq.c:686-710` (Exception-Pfad) → Helper

### 3.3 `sched_preempt` entmüllen — deferred to Phase 7.1

`sched_preempt` ist weiterhin God-Function für Signals + RCU + Alarm + epoll + VT. Reschedule-Kern + typed Frame + Helper: done. Rest via Callback-Registry (Phase 7.1). Begründung: TODO-Scope war Struktur-Sanierung, Per-Process-Alarm-Scan gehört explizit zur Timer-Tick-Callback-Registry.

- [x] FPU-Invariante in frame.h dokumentiert (Kernel kein SSE → keine XSAVE im Sync-Helper)
- [x] FS_BASE-Invariante dokumentiert: schedule()-lifecycle, sigframe-Roundtrip
- [ ] Per-Process-Alarm-Scan `O(PID_TABLE_MAX)` pro Tick → timer_wheel oder RB-Tree (Phase 7.1)
- [ ] `sched_preempt` auf Reschedule + Callbacks schrumpfen (Phase 7.1)

---

## Phase 4 — Stub-Implementierungen ✓ weitgehend abgeschlossen

Commits `01fa5e8` bis `9a0a37a`. 2365/55 → 2398/22, +33 stabile Tests, Varianz ±2.

### 4.1 File-Locks ✓ (+12)

Commit `01fa5e8`. flock_table vereint flock(2) whole-file + fcntl byte-range. flock-Owner = vfs_file* (distinct open(2) descriptions konfligieren), fcntl-Owner = pid. F_SETLK: Typ-Validierung + Pipe-Check, F_SETLKW: Poll-Loop mit thread_block_ms + Signal-EINTR, Range-Splitting + Merge. flock_release_file aus vfs_file_release getriggert.

### 4.2 Capabilities ✓ (+10)

Commit `90cb4c2`. include/linux/capability.h: Versionen + structs. do_capget/do_capset: NULL→EFAULT, unbekannte Version→V3-back + EINVAL, pid<0→EINVAL, pid>0 nonexistent→ESRCH, capset pid>0!=self→EPERM. Single-User liefert alle Caps ~0.

### 4.3 execveat ✓ (+4)

Commit `bbbb68b`. do_execveat in process_exec.c. AT_EMPTY_PATH/AT_SYMLINK_NOFOLLOW, andere Flags→EINVAL. dirfd-Resolution: AT_FDCWD via do_execve, real dirfd baut DIR-Pfad + relativer Pfad zusammen. Symlink + AT_SYMLINK_NOFOLLOW via vfs_lookup_nofollow→ELOOP.

### 4.4 fallocate ~ (bereits aus Phase 1)

Die vorhandene Implementierung (Phase 1.1) lieferte alle Test-Grüne (fallocate01-03). Echte PUNCH_HOLE/ZERO_RANGE-Semantik nicht implementiert — kein fehlschlagender Test fordert das. Falls später benötigt: mode-spezifisches -EOPNOTSUPP pro Kombination.

### 4.5 xattr ✓ (+1)

Commit `8a7f8e6`. fd-basiert: EBADF first, flistxattr(size=0)→0, fgetxattr→ENODATA, sonst ENOTSUP. Path-basiert: Lookup-Errno first, listxattr(size=0)→0, getxattr→ENODATA, sonst ENOTSUP.

### 4.6 eventfd EFD_SEMAPHORE ✓ (+5)

Commit `91fb558`. EFD_SEMAPHORE=0x01. Read: val=1, counter -= 1. Counter=0: EAGAIN/block.

### 4.7 chroot ✓ (+1 stabil)

Commit `8b9d8fa`. process_t.root[256]. resolve_path prependet p->root an normalisierten absoluten Pfad. do_chroot setzt p->root, resettet cwd="/". Chroot02-Test grün, andere chroot-Tests waren bereits grün.

### 4.8 acct ✓ (+4 via Phase 6.6)

Commit `5d3ba99` nach Phase 6.6. path==NULL disabled accounting, path!=NULL validiert via copy_path_from_user + resolve_path + vfs_lookup_err (EISDIR bei Verzeichnis). 4 von 5 acct-Tests grün (efault, enoent, enotdir, null); acct(".") eisdir bleibt rot (resolve_path-Detail).

### 4.9 fadvise64 ✓

Commit `9a0a37a`. POSIX_FADV_* Konstanten aus include/linux/fcntl.h; war schon korrekt implementiert (Phase 1), nur Magic-Numbers ersetzt.

---

## Phase 5 — Loopback-Vollendung ✓ done

Commits `9543eae`, `9606617`, `b10a495`, `0188169`. Test-Delta 2403/16 → 2405/14 (+2 stabil, Varianz ±1 durch externen HTTP-Test).

- [x] `net/lo.c` auf aktuelle `netif`-API portieren (bereits durch frühere Commits)
- [x] 127.0.0.0/8 Routing: direkter `netif_tx` → loopback-netif, kein Gateway-Check
- [x] `connect` lokaler listener: TCP-Statemachine durch Loopback
- [x] `connect` auf unbekannten Port → `-ECONNREFUSED` (RST an closed-port, Commit `9606617`)
- [x] `connect` auf bereits verbundenen Socket → `-EISCONN` (Commit `9543eae`: post-ESTABLISHED-States in `net_tcp_connect` als „connected" behandeln statt neuen SYN-Mzero)
- [x] **Accept-Deadline entfernt** (Commit `b10a495`): `do_accept4` honoriert `SO_RCVTIMEO`, sonst `remain=-1` = indefinite. Per-Listener `wait_thread` ersetzt globalen `q_tcp_wait_thread` — verhindert dangling pointer bei SIGKILL-killed accept.
- [x] ARP non-blocking: NUD-State-Machine + Pending-Queue (Commit `0188169`). Sync-API (`net_arp_resolve`) bounded 250ms. Async-API (`net_arp_queue_frame`) queued das Frame, Reply-Handler flusht.
- [x] Tests grün: `ltp/connect-eisconn`, `ltp/connect01-econnrefused`; bestehende `net/accept-loopback`, `ltp/accept02-loopback` stabil.

### Gefährliche Stellen (entdeckt während der Umsetzung)

- **`mzero(c, sizeof(*c))`** in `net_tcp_accept` und `net_tcp_connect` clobberte `c->wait_thread`. Recursive loopback-RX (send_syn → tcp_input → event_post) brauchte den Wert. Fix: save/restore um den mzero herum.
- **`q_tcp_wait_thread` als globaler Slot** war latente UAF: SIGKILL-killed accept ließ den Zeiger auf eine freigegebene `thread_t` dangling. Nächster tcp_input dereferenzierte sie. Ersetzt durch per-Listener-Slot (`ls->tcp.wait_thread`) mit Socket-Lifetime.
- **RST-Bounce-Loop**: frühe Version sendete RST auch auf RST. Fix: `in_flags & 0x04` → drop before RST-Send.

---

## Phase 6 — Race/Signal-Pfad

**Setzt Phase 3 voraus.** Ohne vereinheitlichten Frame-Sync sind diese Bugs nicht reproduzierbar diagnostizierbar.

### 6.1 Signal-Delivery-Reihenfolge ✓ done

Test-Delta 2416/3 → 2420/3 (+4 PASS aus `ltp/alarm07b`, Varianz = 0 über 6× `make test-hw`).

Audit-Ergebnis: Die strukturellen Subtasks waren bereits durch frühere Phasen erschlagen — Phase 6.1 ist eine Verifikation, kein Refactor.

- [x] Handler **vor** userspace-return: `sys_handler` (`dispatch.c:265`) ruft `check_signals_syscall_path` nach jedem Syscall-Dispatch, vor SYSRET. Legacy `INT 0x80`-Pfad (`irq.c:163-172`) delivert ebenso via `check_pending_signals`. Timer-Preempt (`sched.c:243-259`) delivert für den laufenden User-Thread.
- [x] `SA_RESTART`-Rewind an einer Stelle: nur `signal_handler.c:209-222`. `grep -rn SA_RESTART src/` bestätigt — keine Duplikate in `irq.c`, `sched.c`, `signal_frame.c`.
- [x] `parent got SIGALRM` grün: `ltp/alarm07b` war als "hangs" deaktiviert (`test_alarm.c:191`). Nach Reaktivierung läuft der Test durch — `do_nanosleep` (`sys_time.c:64-80`) prüft `deliverable` vor/nach `thread_block_ms`; `check_alarm_timers` (`signal_handler.c:144-149`) weckt den blockierten Thread via `sched_wake`.

### 6.2 Timing ✓ done

Bereits in Phase 1 (Syscall-Validierung) + Phase 6.1 erschlagen. Serial-Log-Verifikation:

- [x] `clock_settime advance` PASS, `time advanced >= 9s` PASS (`ltp/clock_settime01-advance`)
- [x] `clock_nanosleep mono` PASS, `mono: woke at or after target` PASS (`ltp/leapsec01-mono`)
- [x] `clock_nanosleep` PASS, `woke at or after target (no early expiry)` PASS (`ltp/leapsec01`)
- [x] 12 weitere clock_settime/clock_nanosleep-Fehlerpfad-Tests PASS (EFAULT, EINVAL, monotonic-readonly usw.)

Zusätzlich im gleichen Umfeld: `tst_ncpus_proc` CRASH-Fix — `procfs_global_stat` rief `smp_core_count()` (undefiniertes extern, Linker liess NULL-Symbol durch); ersetzt durch `smp_num_cores()`. Auch `clone301-sigusr2` Test-Port-Bug: LTP-Original ignoriert SIGUSR2 vor clone3, unser Port vergass das → Parent wurde durch exit_signal=SIGUSR2 selbst getötet (status=128+12). Beide vorher: 2420/3, jetzt: 2425/1 (Delta +5, -2 Crashes + 5 Subtests).

### 6.3 Fork/Clone

- [ ] `process_fork.c:217-241` Sibling-Freeze: IPI-basiert, nicht manuelles State-Setzen (→ Phase 7, SMP-1 nicht triggernd)
- [ ] `clone` child exit code: Exit-Pfad durchreichen
- [x] Fork-Memory-Vererbung: CoW-Verifikation. `thp/fork`, `COW multi-fork`, `COW fork write`, `THP COW split`, `madv_free fork` alle grün — lazy CoW funktioniert (Parent-PTE read-only stamp + PTE_COW bit + Page-Refcount, do_wp_page im IRQ-Handler `irq.c:366-411`). `ltp/clone06`-Fail war Test-Design-Bug: `volatile int marker = 42` auf Stack gelegt, vom Compiler via `%rsp`-relativ adressiert; bei clone mit custom child-stack zeigt Child-`%rsp` ins frisch allozierte Stack-VMA (leer, nicht in Parent-Frame). Fix: marker als static-global → RIP-relativer Load → testet echte .bss-CoW-Inheritance. Delta 2414/5 → 2415/4.

### 6.4 Memory-Protection — Stack-Guard-Page (teilweise ✓)

Test-Delta 2413/6 → 2414/5 (`ltp/stack_clash` PASS, 5× Varianz = 0).

Design: PROT_NONE-VMA der Größe `STACK_GUARD_SIZE` (= 1 Page) direkt unter `stack_top - RLIMIT_STACK`. Guard liegt am Wachstumslimit, nicht am initialen Stack-Boden — sonst könnte der Stack nicht mehr wachsen. Fault-Handler erkennt sie per `vma->prot == 0` und liefert unkonditional SIGSEGV.

- [x] `STACK_GUARD_SIZE`-Konstante in `config.h`
- [x] `process_exec.c` + `process.c` fügen PROT_NONE-Guard-VMA bei `[stack_top - RLIMIT_STACK - GUARD, stack_top - RLIMIT_STACK)` ein
- [x] `irq.c` Page-Fault: `vma && prot == 0` → explizit `kill_process`; zusätzlich spin_unlock bei fallthrough im vma-Block (Deadlock-Fix)
- [x] `copy_one_vma` trägt Guard-VMA ohne PTE in Child-Tree (automatisch via vma_walk)
- [x] `test/ltp/test_security.c` stack_recurse: `noinline optimize("O0")` + asm-sink, sonst inlinete GCC die Selbstrekursion zu einer Schleife und der Stack wuchs nie
- [x] Validierung: crash/stack_guard, crash/stack, crash/deep_recursion alle grün (separates `make test-crash`-Target); `/proc/self/maps` zeigt `---p`-VMA
- [ ] `meltdown`-Test: Kernel-Memory-Read aus userspace → SIGSEGV (bereits grün via SMAP, siehe ltp/meltdown)

### 6.5 Socket-Readiness-Wakeup ✓ done

Commits `0dda997`, `0fc6741`. Test-Delta 2400/19 (±3) → 2403/16 (Varianz = 0, 5× identisch).

Design-Entscheidung: Kein neuer `sock_wq` per-Socket — bestehende `epoll_wake_all`-Infrastruktur (globale per-core sleeper-Liste in `event/epoll.c`) wiederverwendet. Linux hat per-sk `wait_queue_head_t`; CosmoRT nutzt globale Liste mit fd-readiness-scan nach wakeup. Funktional äquivalent für SMP-1, spart per-socket-Lock-Ordering.

- [x] `do_poll` (`sys_event.c`) registriert sich via `epoll_sleeper_add_ext` **vor** readiness-check; `epoll_sleeper_remove_ext` am Return (iteriert alle Cores wg. Thread-Migration)
- [x] `epoll_sleeper_add`: dup-check verhindert Listen-Wachstum bei Loop-Iteration
- [x] `lo_send` ruft `epoll_wake_all()` — vorher weckte Loopback-RX nur `c->wait_thread`, kein Poll-Signal
- [x] Accept-Pfad: TCP-Hash-Eintrag sauber von `ls->tcp` auf `ns->tcp` übertragen (`tcp_unregister` + `kmemcpy` + `tcp_register` + `kmemset`, IRQ-disabled). Ohne Fix zeigte der Hash nach `kmemset` auf genullte struct → Folgepakete verloren
- [x] Socket-blocking-Pfade (`connect`/`accept`/`recv`/`recvfrom`/DGRAM-recv): prepare_to_wait-Pattern — `wait_thread` **vor** readiness-check setzen. Behebt lost-wakeup wenn Daten zwischen check und `event_wait` ankommen
- [x] `net_arp_resolve`: Per-Iteration signal-check. Pending deliverable signal → `-1` statt weiter busy-waiten. Timeout bei 3s belassen (NIC-tests brauchen das). Vollständige non-blocking neighbour-cache: Phase 5.
- [x] Validierung: 5× `make test-hw` → identisch 2403/16, Varianz = 0.
- [x] `net/nonblock-connect`, `-read`, `-write` stabil PASS; `ltp/accept02-loopback` 5/5.

### 6.6 Page-Fault-Recovery via Exception-Table ✓ done

Commits `2eaad4f`, `57a2952`, `6421623`, `7e53d07`, `88b6e3a`, `5d3ba99`, `0741ca8`. Test-Delta 2398/22 → 2400/19 (+4 acct, -1 varianz-artefakt).

- [x] Linker-Script (`src/boot/efi_x86_64.lds`): `.ex_table` in `.rodata`, `__start_ex_table`/`__stop_ex_table`, `.text.fixup` nach `.text`
- [x] `_ASM_EXTABLE`-Macro in `include/kernel/mm/extable.h`, Linux-kompatibles 12-byte-Layout (3× rel32)
- [x] Binary-Search in `src/kernel/mm/extable.c`, Boot-Sort (insertion) via `extable_sort()` aus `kmain`
- [x] Page-Fault-Handler (`src/kernel/core/irq.c`): extable-Lookup vor Legacy-Pfad bei Kernel-Mode-Fault; auch im `default_exception_with_frame` für GPF/fxrstor
- [x] `copy_from_user`/`copy_to_user` als echte Funktionen in `src/kernel/mm/uaccess.c` mit `rep movsb`+`_ASM_EXTABLE`
- [x] `copy_path_from_user` (`dispatch.c`) und `copy_path_from_user_proc` (`process_exec.c`) auf byte-weisen extable-Asm migriert
- [x] **Kern-Fix (Wurzel-Korrektur)**: Page-Fault-Handler Kernel-Mode demand-paged **nicht mehr** PROT_NONE-VMAs — das war die tatsächliche Ursache der 140-Test-Regression, **nicht** die in dieser TODO angenommene setjmp/longjmp-State-Corruption. User-Pfad (`irq.c:443`) hatte den Guard, Kernel-Pfad (`:220`) fehlte ihn. Kernel-Zugriff auf PROT_NONE-User-Page allozierte Page mit `prot=0` ins User-VMA → kaskadierende fork/COW-Alloc-Failures. Einzeiler `knp &&= (kprot & (PROT_READ|PROT_WRITE|PROT_EXEC))`.
- [x] extable-Refactor ist unabhängige Struktur-Sanierung (sauberer als setjmp/longjmp, robuster für künftige `copy_*_user`-Pfade), aber allein hätte er die Regression nicht gelöst.
- [x] `fault_jmpbuf`/`fault_recover` aus `thread_t` entfernt, `kernel_setjmp`/`kernel_longjmp` aus `context.S`, `sys_handler`-Wrap entfernt
- [x] Validierung: 4 acct-Tests grün (Phase 4.8 `[x]`), keine Regression.

---

## Phase 7 — Architektur-Schulden

Kontinuierlich, parallel zu Phasen 4-6.

### 7.1 Timer-Tick-Callback-Registry ✓ done

Commits `7cc77de`, `7277835`, `e53b8f8`, `4ccad5f`. 2425/1 stabil (5×, Varianz 0).

- [x] `core/tick.c`: `tick_register(cb, fn, interval_ns)` + `tick_run(now_ns)`
- [x] Registriert: `epoll_check_timeouts`, `check_alarm_timers`, `vt_flush`-Wrapper, `serial_bridge_poll`, `net_rx_poll`-Wrapper, `net_tx_poll`-Wrapper
- [x] `timer_handler` iteriert Registry via `tick_run(ticks * 1e6)`; läuft bei Vektor 32 VOR `sched_preempt`, damit Signal-Delivery-Latenz 1 Tick bleibt
- [x] `sched_preempt` reduziert auf Signal-Delivery + RCU + Timeslice + `schedule()`. Zeilenzahl 331 → 290.
- [ ] Folge-Arbeit: Per-Prozess-Alarm-Scan in `check_alarm_timers` ist weiterhin O(PID_TABLE_MAX)=4096 pro Tick — Timer-Wheel/RB-Tree ist Phase 7.3-Nachbar.

### 7.2 HAL durchgesetzt ✓ done

Entscheidung: **HAL behalten und durchsetzen** — aarch64 ist geplantes Zweitziel (CLAUDE.md), HAL ist die einzige Plattformgrenze. Rückweg (Layer löschen) würde aarch64-Port zur Rewrite machen.

- [x] HAL-Interface ergänzt: `hal_cpu_halt_noirq`, `store_release/load_acquire`, `stack_ptr`, `hwrand`, `user_access_begin/end`, `shutdown`, `reset`, `set_percpu_active`, `fpu_boot_init`, `io_{outb,inb,outl,inl}`; `hal_mmu_switch` (paddr), `flush`, `flush_all`, `flush_range`, `fault_address`; `hal_irq_install_vector_table`
- [x] `src/kernel/` komplett auf `hal_*` migriert: 0 `arch_*`-Calls (außer Linux-ABI-Syscall-Name `arch_prctl` in Kommentar/Tabelle), 0 inline-asm, 0 `#ifdef __x86_64__`
- [x] Inherent x86-spezifische Quellen nach `src/arch/x86_64/` verschoben: `core/timer.c` (PIT/CMOS) → `timer/timer.c`; `core/tss.c` (TSS+SYSCALL-MSR) → `cpu/tss.c`; `mm/uaccess.c` (extable-rep-movsb/byte-copy) → `cpu/uaccess.c`; `copy_path_from_user` konsolidiert (war 2× dupliziert)
- [x] Boot-CPU-Feature-Detection (`CR0/CR4/CPUID/XSETBV`) aus `core/main.c` nach `arch/x86_64/cpu/hal_features.c` als `hal_cpu_fpu_boot_init()` + `memops_init()`
- [x] aarch64-HAL-Stubs in `src/arch/aarch64/hal_{cpu,mmu,irq,timer,smp}.c` — jede Funktion panic-trap, ready für Phase 9. Nicht in x86_64-Build gelinkt.
- [x] Test-Stand 2425/1 stabil über alle 4 Migrations-Commits.

### 7.3 Fixe Pools → Slab + RLIMIT

CLAUDE.md `Ressource-Design` (Wurzel): fixe systemweite Pools sind **verboten**. Jede Ressource muss Slab-allokiert, per-Prozess gecapped (RLIMIT), on-demand wachsend sein. "Pool vergrößern" ist keine Option — bleibt Angriffsvektor.

**Stand (Commits 67048b5, 2de6902, 057a785, 6db7323, 36268ba, 36529f0, 7bfb253, a627dc5, d96c95f):** Kritisch-Pools vollständig migriert. Mittel-Hoch teilweise. Test-Stand 2425/1 stabil über alle 9 Commits.

#### Kritisch (systemweit, DoS-anfällig)

Einzelner Prozess kann alle Slots aufbrauchen → blockiert alle anderen.

| Pool | Wert | Datei | Linux-Äquivalent | Fix |
|------|------|-------|------------------|-----|
| `FD_MAX` | 1024 | `event/fd.h:33` | `expand_fdtable()` 32→256→∞ | dynamisch, RLIMIT_NOFILE |
| `PIPE_MAX` | 32, Buf 4KB | `sys/sys_ipc.c:9` | Slab, Default 64KB | Slab, Buffer 64KB, RLIMIT |
| `NET_TCP_MAX` | 256 | `net/tcp.h:16` | unbegrenzt, adaptive OOO | Slab |
| `NET_MAX_SOCKETS` | 256 | `net/socket.h:8` | unbegrenzt | Slab |
| `USOCK_MAX` | 32 | `net/unix_socket.h:17` | unbegrenzt, skb-Queues | Slab |
| `ACCEPT_QUEUE_MAX` | 8 | `net/socket.h:18` | Default 128, per `listen()` | dynamisch per listen-backlog |
| TCP OOO-Queue | 4 Slots | `net/tcp.h` | adaptive | Slab pro Connection |

#### Mittel-Hoch (funktional einschränkend)

| Pool | Wert | Datei | Linux-Äquivalent | Fix |
|------|------|-------|------------------|-----|
| `PID_TABLE_MAX` | 4096 | `proc/process.c:18` | IDR/IDA bis 4M | Radix-Tree |
| `VMA_MAX` | 8192 | `mm/vma.c:7` | dyn. Slab, unbegrenzt | Slab-Wachstum |
| `PTY_MAX` | 12 | `vt/pty.h:13` | dyn., typisch 256+ | Slab |
| `UDP_POOL_SIZE` | 128 | `net/udp.h:13` | unbegrenzt | Slab |
| `TW_MAX_TIMERS` | 256 | `core/timer_wheel.h:15` | hier. Wheel unbegrenzt | Wheel-Wachstum |
| `EQ_MAX_EVENTS` | 16 Ring/Thread | `core/event_queue.h:39` | unbegrenzte Wake-Lists | Ring-Wachstum bei Overflow |
| `MOUNT_MAX` | 16 | `fs/vfs.h:138` | dyn. Mount-Tree | Slab-Liste |
| `ARP_POOL_SIZE` | 128, Hash 64 | `net/arp.h:11` | `neigh_table`: Slab+Hash, gc_thresh1/2/3=128/512/1024 | Slab + adaptive Hash, GC-Thresholds |
| `EXECVE_MAX_ARGS/ENVS/STRLEN` | 256/256/**4KB total** | `proc/proc_internal.h:58-60` | ~131072 args, 128KB/string, MB total | **Refactor**: Stack-Arrays → page-by-page-Kopie auf User-Stack-Page |
| `HW_MAX_HANDLERS_PER_IRQ` | 4 | `hw/cosmort.c:125` | `irqaction`-Liste unbegrenzt | verkettete Liste pro IRQ |
| `EQ_LOCK_MAX` | 512 | `core/event_queue.c:19` | per-thread Lock | per-thread Lock statt Hash |

#### Niedrig (Ausnahmen erlaubt)

CLAUDE.md-Ausnahmen: Hardware-erzwungen oder POSIX-definiert. Bleiben wie sie sind, aber dokumentieren.

| Limit | Wert | Begründung |
|-------|------|------------|
| Signal-Actions | 64 | Linux-ABI (`_NSIG=64`, signals 1-64) |
| IDT | 256 | x86_64 ISA-Vorgabe |
| IRQ-Handler-Tabelle | 256 | IDT-Vektor-indiziert, Hardware |
| Hostname | 64 | Linux `HOST_NAME_MAX=64` |

#### Umsetzungs-Reihenfolge

Jede Migration eigener Task, Reihenfolge nach DoS-Risiko (kritisch zuerst):

**Kritisch — erledigt:**

- [x] `FD_MAX` → per-Prozess 1024-Slot-Array **und** `RLIMIT_NOFILE`-Enforcement in `fd_alloc` (process.c:239). Tabelle ist per-Prozess, nicht systemweit → kein DoS-Vektor. Echte dynamische Expansion (Linux-fdtable-Style, 32→64→128→∞) bleibt Follow-up: aktuell fix 1024, das ist für Single-User-Alpine+Apps ausreichend.
- [x] `PIPE_MAX` → dynamischer Slab; Encoding read/write-End via `fde->flags & O_WRONLY` (ersetzt `pp+1`-Pointer-Hack). `fd_obj_incref` bekommt flags als dritten Parameter. **Commit 057a785**.
- [x] `NET_TCP_MAX=256` → war totes Makro, entfernt. TCP-Verbindungen haengen am `sock_slab` (jetzt dynamisch). **Commit 36268ba**.
- [x] `NET_MAX_SOCKETS=256` → `sock_slab` auf `slab_init_dynamic` umgestellt. **Commit 2de6902**.
- [x] `USOCK_MAX=32` → dynamischer Slab + intrusive Active-List fuer bind/connect-Path-Lookup. **Commit 67048b5**.
- [x] `ACCEPT_QUEUE_MAX=8` → war Dead-Code (`accept_count` wurde nirgends inkrementiert, Queue nie befuellt). Komplett entfernt. **Commit 6db7323**.

**Mittel-Hoch — erledigt:**

- [x] `VMA_MAX=8192` → `vma_slab` auf dynamic umgestellt (384KB .bss eliminiert). **Commit 7bfb253**.
- [x] `MOUNT_MAX=16` → Slab + sortierte Linked-List (longest-prefix-first). **Commit 36529f0**.
- [x] `ARP_POOL_SIZE=128` → dynamischer Slab, `evict_one`-Fallback bleibt fuer OOM. **Commit a627dc5**.
- [x] `HW_MAX_HANDLERS_PER_IRQ=4` → irqaction-Liste pro IRQ, dynamischer Slab. **Commit d96c95f**.

**Mittel-Hoch — offen (Follow-up):**

- [ ] `UDP_POOL_SIZE=128` → Migration auf dynamischen Slab versucht, reproduzierbare Regression in DNS-Tests (recvfrom flakey) — Ursache unklar, vermutlich timing-abhaengig mit externen slirp-DNS. Revertiert.
- [ ] `PID_TABLE_MAX=4096` → Radix-Tree/IDR (aufwaendig; 4096 ist praktisch genug).
- [ ] `TID_TABLE_MAX=4096` → wie PID.
- [ ] `PTY_MAX=12` → Slab (Signatur-Aenderung: `pty_get(int id)` vs. Pointer-Rueckgabe).
- [ ] `TW_MAX_TIMERS=256` → Index-basiertes Wheel auf Pointer/list_head umbauen.
- [ ] `EQ_MAX_EVENTS=16` → per-Thread, nicht systemweit — kein DoS-Risk. Ring-Wachstum bei Overflow.
- [ ] `EXECVE_MAX_*` → 128KB-Buffer ist Linux-kompatibel (`ARG_MAX`), keine Aktion noetig.
- [ ] `EQ_LOCK_MAX=512` → struktureller Umbau, per-thread Lock.
- [ ] `_Static_assert` auf kritische Slab-Struct-Groessen.
- [ ] `EXT4_OPEN_MAX=256` (`fs/vfs.c:283`) → lineare Suche unter Lock. Hash-Table oder in `struct ext4_inode`-Cache integrieren. Aus 7.4-Audit abgeleitet.

**RLIMIT-Infrastruktur:**

- [x] `prlimit64` implementiert fuer `RLIMIT_NOFILE`, `RLIMIT_STACK`, `RLIMIT_DATA`, `RLIMIT_AS`. Enforcement fuer `RLIMIT_NOFILE` in `fd_alloc`.
- [ ] `RLIMIT_NPROC`, `RLIMIT_FSIZE`, `RLIMIT_CPU` — noch nicht verdrahtet.

### 7.4 Lock-Granularität — audit-only, deferred bis SMP-N

`SMP_MAX_CORES=1` hart verdrahtet (`include/kernel/config.h:33`). Alle vier Locks sind
im aktuellen Single-Core-Kontext **unkritisch** (keine Contention möglich). Migration
ohne SMP-N ist reiner Mock: per-CPU-Strukturen degenerieren zum globalen State, Split-
Varianten (z.B. per-order buddy_lock) führen Deadlock-Potential ein ohne messbaren
Gewinn. Follow-up nach Phase 9 (aarch64-Port) sobald SMP-N aktiviert wird.

**Audit (Reihenfolge nach Kosten bei SMP-N-Aktivierung):**

| Lock | Datei | Callsites | Op-Dauer | Linux-Referenz | Migration-Aufwand |
|------|-------|-----------|----------|----------------|-------------------|
| `buddy_lock` | `mm/page_alloc.c:27` | 8 (alloc/free/huge) | O(Order-Listen-Op) + Bitmap-Scan | `mm/page_alloc.c`: PCP (per-CPU pageset) für Order-0 hot/cold; `zone->lock` für Buddy-Merge | ~200 LOC: `struct per_cpu_pages`, refill/drain von Order-0-Batches, Fallback auf globalen Lock für Order≥1 und Merge |
| `fs_lock` | `fs/ext4.c:27` | 7 (block/inode alloc/free) | O(Group-Count × Bitmap-Byte-Scan) | `struct inode.i_rwsem` + `block_group.bg_lock` | ~300 LOC: per-Block-Group-Lock für Bitmap-Scans, per-Inode rwsem für read/write. Erfordert ext4-Inode-Cache-Refactor. |
| `cache_lock` | `fs/bcache.c:23` | 4 (get/put/sync/write) | O(Hash-Chain + LRU-Move) | `struct buffer_head.b_state` (atomic bits) + Hash-Bucket-Locks | ~200 LOC: pro-Hash-Bucket-Lock, atomic `BH_Lock` für I/O-Flag, separate LRU-Lock. Racy-LRU-Mutation vermeiden. |
| `s->lock` (slab) | `mm/slab.c`:`slab_t` | pro Slab-Instanz | O(1) Free-List-Op | `struct kmem_cache_cpu` Magazine (Solaris-Pattern) | ~150 LOC pro Slab: per-CPU Magazine mit lockfree Push/Pop, shared Depot-Lock nur bei Refill. |

**Migration-Trigger:** Erst nach Phase 9 (aarch64-Port) und `SMP_MAX_CORES > 1`.
Reihenfolge dann: slab magazine (lokal testbar) → buddy PCP (Hot-Path-Messbar) →
bcache bucket-locks → fs_lock per-inode. Jeder Lock erhält in der Migration eigenen
Commit mit Microbenchmark-Messung gegen SMP-1-Baseline.

**Verifizierbarer Nebenbefund bei Audit:** `ext4_open_lock` (`fs/vfs.c:284`) ist
nicht in dieser Liste, hat aber **lineare Suche** über `EXT4_OPEN_MAX=256`-Array
unter Lock. Separat aufzulösen durch Hash-Table — wird zu Phase 7.3 Mittel-Hoch
hinzugefügt, nicht zu 7.4.

### 7.5 RCU-Vollendung ✓ done

- [x] Callback-Execution aus `rcu_gp_complete` deferred — Tick-basiert statt dedizierter kthread: `rcu_tick_deferred` registriert via `tick_register(TICK_EVERY)`, drained `cb_done_*` pro CPU und weckt `sync_ready`-Waiter. Trennt Callbacks von `rcu_read_unlock`/`schedule`/`synchronize_rcu`-Pfaden. Max-Latenz 1 Tick.
- [x] `rcu_cpu_of_self()` Helper clampt `core_id` auf `[0, SMP_MAX_CORES)` an allen vier Aufrufstellen (`rcu_gp_complete`, `rcu_note_context_switch`, `rcu_check_callbacks`, `call_rcu`).

### 7.6 Layer-Verstöße ✓ done

Mit Phase 7.2 erledigt:

- [x] `sys/sys_proc.c` reboot → `hal_cpu_reset()` (Triple-Fault-Impl in `arch/x86_64/cpu/hal_cpu.c`)
- [x] `hw/serial.c` outb/inb → `hal_io_outb`/`hal_io_inb`
- [x] `sched.c` + alle weiteren `arch_*`-Calls in `src/kernel/` → `hal_*`

Übrig: `core/irq.c` enthält noch APIC-Register-Direkt-Access (LAPIC/IOAPIC MMIO) und IDT-Entry-Tabelle. Der IRQ-Init-Block (handler-table, APIC-Programmierung) ist x86-spezifisch und gehört nach `src/arch/x86_64/irq/` — **separater Phase-7-Umzug**, nicht blockierend für aarch64-Interface.

---

## Phase 8 — Fehlende Subsysteme

### 8.1 Audio — Kern-Identität

`notes/NOTES.md` sagt 0%. CosmoRT ist Audio-Realtime-Kernel.

- [ ] `drivers/audio/hda.c`: Intel HDA-Treiber
- [ ] `drivers/audio/virtio_snd.c`: QEMU virtio-sound
- [ ] `drivers/audio/audio.c`: Ring-Buffer, `/dev/snd/*`
- [ ] ALSA-ABI oder eigene minimal-API (`notes/AUDIO.md` entscheiden)
- [ ] RT-Scheduling-Pfad bis Audio-Thread bounded

### 8.2 Test-Runner-Robustheit → siehe Phase 0.5

Nach dem Netz-Audit vorgezogen. Inhalt in Phase 0.5 dokumentiert.

### 8.3 procfs-Vervollständigung — abgeschlossen

- [x] procfs mmap-Pfad: FD_PROCFS -> eager copy in anonyme Pages (fix: `mmap file`)
- [x] `/proc/stat`: 10 CPU-Felder + intr/procs_running/procs_blocked/softirq
- [x] `/proc/<pid>/status`: 21 Felder (Tgid, Uid, Gid, VmRSS, VmData, VmStk, VmExe, Sig*)
- [x] `/proc/<pid>/stat`: 52 Linux-Felder (vsize/rss/startstack/start_brk/exit_signal)

---

## Restaufgaben (Einzelfixes)

- [x] `acct(".")` → `-EISDIR`: Commit `e5f8d3a` (resolve_path-Fix am Root).
- [ ] `ltp/copy_file_range-basic` "data matches": 31 Bytes geschrieben via copy_file_range, nach lseek(0) read liefert 31 Bytes, aber buf[0]!='A' || buf[30]!='\n'. tmpfs_op_pwrite + tmpfs_op_read sehen korrekt aus; `copy_file_range-offsets` + `copy_file_range03` (parallel code paths) sind grün. Unklare Ursache — ohne trace-Daten nicht lokal.

## Einzelbug-Sweep — 2405/14 → 2413/6 (+8, 5 Bugs)

Commits `0a96060`..`f6b79d1`:
- `adjtimex03`: ADJ_ADJTIME ohne ADJ_OFFSET_SINGLESHOT/SS_READ → EINVAL
- `faccessat202` invalid flags: faccessat2 flag-Validierung
- `epoll_create1-cloexec`, `close_range-cloexec`: CLOEXEC-Propagation + Modus
- `epoll_wait07`: EPOLLONESHOT-Handling (Flags nach Delivery löschen)
- `epoll_ctl02-eperm`: FD_FILE als Target → EPERM
- `execve03-eacces`: Execute-Bit-Check vor ELF-Parse
- `acct("."` EISDIR: resolve_path Dot-Normalisierung am Root

---

## Non-Kernel (nach Phase 1-6 abarbeiten)

### musl libc-test (20 FAIL)

- [ ] `sem_open`: MAP_SHARED-Kohärenz
- [ ] `pthread_robust`: Robust-Futex-Cleanup bei Thread-Exit
- [ ] `malloc-brk-fail`: brk VMA-Overlap-Check
- [ ] `fma`/`fmal`/`powf`/`remquol`: FPU-State Preservation (Kernel-Kontext-Switch)
- [ ] `tls_get_new-dtv`: dlopen/TLS-Setup

### LTP (87 FAIL)

- [ ] `execve`: Execute-Bit prüfen (blockiert 60+ Tests) — erledigt sich mit Phase 1.3
- [ ] VMA/TLB Race: Atomarer VMA-Update + TLB-Shootdown (mit Phase 6.3 kompatibel)

---

## Erledigt (Archiv)

- context_switch Unification (Commits `a6d6cd4`, `dd75276`, `b1031a4`, `9271aaf`, `50472c5`) — einziger Pfad in `arch/x86_64/cpu/context.S`
- Preemptible RCU (Commit `781dfd9`) — Linux PREEMPT_RCU-Modell
- VFS Refactoring, ext4, Netzwerk-Basis (Merge `6395591`) — 2260/79
- Loopback-Basis (Commit `7b85a5f`) — halb fertig, siehe Phase 5
- fadvise64 Validierung (wenn diese Klasse aus aktuellem Fail-Stand verschwunden ist)
- epoll Event-Delivery (alte Klasse D, 14 → aktuell ~6 Fails) — Wakeup-Verdrahtung teilweise erfolgt, Rest in Phase 1.1
