# CosmoRT — TODO

Stand: ktest 2405/14 (5× Median, Varianz ±1 durch externen HTTP-Test `net/tcp_hash_multi`), musl 452/20, LTP 11/87. Branch: `ltp`.

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

## Phase 3 — `sched_preempt`-Refactor

**Begründung:** Blockiert Diagnose der Phase-6-Races. `sched_preempt` hat zwei Frame-Save-Pfade, Frame-Sync ist in vier Stellen dupliziert (sched.c ×2, irq.c ×3 für INT 0x80 / SIGSEGV / Exception). Magic-Indices `f[18] & 3`. FS_BASE asymmetrisch. XSAVE implizit safe nur wegen `-mgeneral-regs-only`.

### 3.1 Frame-Sync-Primitive

- [ ] `static void irq_frame_to_thread(const irq_frame_t *f, thread_t *t)` in `core/frame.c` (neu)
- [ ] `static void thread_to_irq_frame(const thread_t *t, irq_frame_t *f)`
- [ ] `_Static_assert` auf `irq_frame_t`-Layout (alle Offsets die Nutzer brauchen)
- [ ] Typed `irq_frame_t*` durch `sched_preempt` statt `void*`
- [ ] Magic-Index `f[18] & 3` → `f->cs & 3`, Helper `frame_is_user(f)`

### 3.2 Duplikate eliminieren

- [ ] `sched.c:237-251` → `irq_frame_to_thread` + Signal-Delivery + `thread_to_irq_frame`
- [ ] `sched.c:270-290` → dieselben Helper
- [ ] `irq.c:170-196` (INT 0x80) → Helper
- [ ] `irq.c:573-596` (SIGSEGV) → Helper
- [ ] `irq.c:686-710` (Exception-Pfad) → Helper

### 3.3 `sched_preempt` entmüllen

`sched_preempt` ist God-Function für Signals + RCU + Alarm + epoll + VT. Nur Reschedule-Kern behalten, Rest via Callback-Registry (Phase 7.1).

- [ ] FPU-Save/Restore explizit machen oder Invariante dokumentieren (Kernel kein SSE → FPU-berührungsfrei)
- [ ] FS_BASE-Sicherung symmetrisch machen oder Invariante dokumentieren
- [ ] Per-Process-Alarm-Scan `O(PID_TABLE_MAX)` pro Tick → timer_wheel oder RB-Tree

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

### 6.1 Signal-Delivery-Reihenfolge

- [ ] Nach `-EINTR` aus blocking syscall: Handler **vor** userspace-return ausführen
- [ ] `SA_RESTART`-Rewind nur noch an einer Stelle (Phase 3 Frame-Sync)
- [ ] `parent got SIGALRM`-Test: Handler läuft vor Test-Code

### 6.2 Timing

- [ ] `time advanced >= 9s`: `clock_settime` muss wall-clock tatsächlich vorspulen
- [ ] `clock_nanosleep CLOCK_MONOTONIC`: nicht `-EINTR` bei Timer-Expiry, nur bei Signal
- [ ] TSC-Präzision: `woke at or after target`-Regression

### 6.3 Fork/Clone

- [ ] `process_fork.c:217-241` Sibling-Freeze: IPI-basiert, nicht manuelles State-Setzen
- [ ] `clone` child exit code: Exit-Pfad durchreichen
- [ ] Fork-Memory-Vererbung: CoW-Verifikation

### 6.4 Memory-Protection

- [ ] Stack-Guard-Page (1 Page unterhalb jedes user-Stacks, `PROT_NONE`)
- [ ] Test `stack_clash` darf SIGSEGV auslösen
- [ ] `meltdown`-Test: Kernel-Memory-Read aus userspace → SIGSEGV

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

### 7.1 Timer-Tick-Callback-Registry

`sched_preempt` und `timer_handler` haben `extern void`-Salat: `epoll_check_timeouts`, `check_alarm_timers`, `vt_flush`, `serial_bridge_poll`, `net_rx_poll`, `net_tx_poll`. Linux-Modell: registrierte Callbacks.

- [ ] `core/tick.c`: `tick_register(fn, interval_ns)`
- [ ] Subsysteme registrieren sich in ihren `*_init()`-Funktionen
- [ ] `timer_handler` iteriert Registry, ruft fällige Callbacks
- [ ] `sched_preempt` verliert alle `extern void` außer Reschedule-Kern

### 7.2 HAL: echt oder löschen

`hal_cpu.c:17-44` ist Pure-Forwarding. `src/kernel/` ruft `arch_*` direkt 7× (sched.c). Entweder HAL ernst nehmen (Kernel ruft nur `hal_*`) oder Layer löschen.

- [ ] Entscheidung: HAL behalten und durchsetzen, oder HAL entfernen?
- [ ] Wenn behalten: jeden `arch_*`-Call in `src/kernel/` durch `hal_*` ersetzen
- [ ] Wenn entfernen: `hal/`-Layer komplett, alle `arch_*` direkt exponieren

### 7.3 Fixe Pools → Slab + RLIMIT

CLAUDE.md `Ressource-Design` (Wurzel): fixe systemweite Pools sind **verboten**. Jede Ressource muss Slab-allokiert, per-Prozess gecapped (RLIMIT), on-demand wachsend sein. "Pool vergrößern" ist keine Option — bleibt Angriffsvektor.

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

- [ ] `FD_MAX` → expand_fdtable + RLIMIT_NOFILE
- [ ] `PIPE_MAX` → Slab + 64KB-Buffer + RLIMIT_NOFILE (FDs zählen die Pipes)
- [ ] `NET_MAX_SOCKETS` + `NET_TCP_MAX` + TCP-OOO → Slab
- [ ] `USOCK_MAX` → Slab
- [ ] `ACCEPT_QUEUE` → listen-backlog-Argument respektieren, dyn. Array
- [ ] `PID_TABLE_MAX` → Radix-Tree/IDR
- [ ] `VMA_MAX`, `PTY_MAX`, `UDP_POOL_SIZE`, `TW_MAX_TIMERS`, `EQ_MAX_EVENTS`, `MOUNT_MAX` → Slab-Wachstum
- [ ] `ARP_POOL_SIZE` → Slab + adaptive Hash nach `neigh_table`-Modell, gc_thresh-sysctl, Test `test_arp_cache.c` anpassen (Pool-Overflow-Test wird bedeutungslos)
- [ ] `EXECVE_MAX_*` → Linux-Modell: argv/envp page-by-page aus User kopieren in eine Übergangs-Page, dann in neuen Prozess-Stack. Kernel-Stack-Arrays raus. Limits: `ARG_MAX` = min(128KB, RLIMIT_STACK/4)
- [ ] `HW_MAX_HANDLERS_PER_IRQ` → `irqaction`-Liste (list_head) pro Vektor, `request_irq`/`free_irq`-Semantik
- [ ] `EQ_LOCK_MAX` → per-thread Lock (struktureller Umbau, nicht Slab)
- [ ] RLIMIT-Infrastruktur: `setrlimit`/`getrlimit`/`prlimit64` verdrahten (heute Stub?)
- [ ] `_Static_assert` auf kritische Slab-Struct-Größen (Cache-Line-Alignment)

### 7.4 Lock-Granularität

- [ ] Per-CPU Page Freelists (`mm/page_alloc.c`): `buddy_lock` → per-CPU + Steal
- [ ] Per-Inode `rw_semaphore` (`fs/vfs.c`): `fs_lock` aufbrechen
- [ ] Per-Block atomare Flags (`fs/bcache.c`): globales Lock eliminieren
- [ ] Per-CPU Slab-Freelist (`mm/slab.c`): Magazine-Pattern

### 7.5 RCU-Vollendung

- [ ] Callback-Execution aus `rcu_gp_complete` in dedizierten Kernel-Thread (aktuell: synchron im caller-Kontext, kann `synchronize_rcu`-Pfad blockieren)
- [ ] `rcu_state.cpu[i]` Range-Check auf `SMP_MAX_CORES` (`rcu.c:262,283,297`)

### 7.6 Layer-Verstöße

- [ ] `src/kernel/sys/sys_proc.c:235-236`: inline-asm `lidt`/`int3` für reboot in `arch/x86_64/` verschieben
- [ ] `src/kernel/hw/serial.c`: inline-asm `outb/inb` → `hal_io`
- [ ] Jeden `arch_*`-Call in `src/kernel/` auditieren (sched.c:150,157,162,163,166,243,275)

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

### 8.3 procfs-Vervollständigung

- [ ] `/proc/stat`: CPU-Zeilen vollständig (user/nice/sys/idle/iowait/irq/softirq)
- [ ] `/proc/<pid>/status`, `/proc/<pid>/stat`: fehlende Felder

---

## Restaufgaben (Einzelfixes)

- [ ] `acct(".")` → `-EISDIR`: aktuell `-ENOENT`. `resolve_path`-Detail bei CWD="/" + path=".": liefert Pfad den `vfs_lookup_err` nicht findet statt den Root-Inode. Kein 6.6-Issue, isolierter VFS-Lookup-Bug.

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
