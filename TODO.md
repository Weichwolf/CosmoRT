# CosmoRT — TODO

Stand: ktest 2361/58, musl 452/20, LTP 11/87. Branch: `ltp`.

Priorisierung aus Architektur-Audit. Reihenfolge ist bindend: spätere Phasen setzen frühere voraus.

## Phasen-Übersicht

| # | Phase | Fails adressiert | Aufwand | Risiko | Blocker für |
|---|-------|------------------|---------|--------|-------------|
| 0 | Build-Infrastruktur (Header-Deps) | **✓ done** (Commit `5d17930`, 2335/79 → 2341/78) | — | — | — |
| 1 | Syscall-Validierung (Klasse D) | **✓ done** (2334/79 → 2349/65) | — | — | — |
| 2 | VFS-Metadaten (Klasse B) | **✓ done** (2354/65 → 2361/58, Commit `560157d`) | — | — | — |
| **0.5** | **Test-Runner-Watchdog (vorziehen)** | Test-Count-Varianz (±5) → 0 | 1 Tag | niedrig | 6.5-Diagnose |
| 3 | `sched_preempt`-Refactor | 0 direkt | 3 Tage | mittel | Phase 6 |
| 4 | Stub-Implementierungen (Klasse A) | ~25 | 5 Tage | niedrig | — |
| 5 | Loopback-Vollendung (Klasse C) | ~10 | 2 Tage | mittel | — |
| 6 | Race/Signal-Pfad (Klasse E) + **6.5 Socket-Readiness-Wakeup** | ~10 + 4 Netz | 4+3 Tage | **hoch** | — |
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

## Phase 0.5 — Test-Runner-Watchdog (vorziehen, blockiert 6.5-Diagnose)

`make test-hw` 5× sequentiell gelaufen (Netz-Audit nach Phase 2):

| Test | PASS-Quote |
|------|-----------|
| `net/nonblock-connect` | 2/5 |
| `net/nonblock-read` | 2/5 |
| `net/nonblock-write` | 0/5 (stabil rot) |
| `ltp/accept02-loopback` | 4/5 |

Runner setzt `alarm(5)` im Child (`test/main.c:54`). Kernel-Busy-Waits in `net_arp_resolve` (bis 3s, `arp.c:209`) + `dhcp.c:39` (3s Retry). Summiert mit slirp-NAT-Delay → SIGALRM-Race **vor** echter Kernel-Fehlschlag sichtbar wird.

Ohne diesen Fix sind 6.5-Kandidaten als "Flake" getarnt. Mit Fix wird der echte Bug deterministisch rot.

- [ ] Parent-seitiger `fork`+`waitpid`-Watchdog mit `SIGKILL` on timeout
- [ ] `alarm()` im Child entfernen
- [ ] Timeout pro Kategorie konfigurierbar (unit=2s, net=10s, fork=5s)
- [ ] Löst die 4 alarm-Tests (ehemalige Klasse M)
- [ ] Test-Count-Varianz muss auf 0 fallen (5× Run-Delta = 0)

**Nicht** in Phase 8.2 — zu früh nötig für die Netz-Diagnose.

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

## Phase 4 — Stub-Implementierungen

Jede Sektion eigenständig.

### 4.1 File-Locks (7+5 Failures)

`fs/locks.c` nach Linux-Vorbild. `do_flock` ist no-op. F_SETLK validiert nicht.

- [ ] `fs/locks.c` + `fs/locks.h` neu
- [ ] `struct file_lock` pro VFS-Node (per-inode list)
- [ ] `flock`: LOCK_SH / LOCK_EX / LOCK_UN / LOCK_NB
- [ ] `flock`: `LOCK_SH|LOCK_EX` → `-EINVAL`
- [ ] `flock`: `LOCK_NB` alleine → `-EINVAL`
- [ ] `flock(-1, ...)` → `-EBADF`
- [ ] `fcntl F_SETLK`: Lock-Typ validieren, Pipe → `-EINVAL`
- [ ] `fcntl F_SETLK` partial-unlock korrekt (range-splitting)
- [ ] `fcntl F_SETLKW`: blockieren + Signal-Wakeup → `-EINTR`
- [ ] `close` löscht alle Locks des Prozesses auf dem Inode

### 4.2 Capabilities (10 Failures)

Nach `kernel/capability.c`. Single-User: alle Caps gesetzt, trotzdem korrekte ABI.

- [ ] `do_capget`/`do_capset` implementieren (bisher `-EPERM`-Stub)
- [ ] Version-Header-Parsing (`_LINUX_CAPABILITY_VERSION_1/2/3`)
- [ ] `kernel_cap_t` in `task_struct` (effective/permitted/inheritable/bounding/ambient)
- [ ] Bad version → `-EINVAL`, Kernel-expected-version zurückgeben
- [ ] `pid < -1` → `-EINVAL`
- [ ] nonexistent pid → `-ESRCH`
- [ ] NULL `data` → `-EFAULT`

### 4.3 `execveat` (4 Failures)

- [ ] `fs/exec.c:do_execveat_common`-Equivalent
- [ ] dirfd-Resolution (AT_FDCWD, absoluter Pfad, relativer Pfad)
- [ ] AT_EMPTY_PATH / AT_SYMLINK_NOFOLLOW
- [ ] Invalid flags → `-EINVAL`
- [ ] Bad dirfd → `-EBADF`
- [ ] `notdir` → `-ENOTDIR`
- [ ] Symlink-Loop → `-ELOOP`

### 4.4 `fallocate` Flags (Klasse A + T.184)

- [ ] `FALLOC_FL_KEEP_SIZE`: File-Size konstant, Block allokieren
- [ ] `FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE`: Range auf 0 setzen
- [ ] `FALLOC_FL_ZERO_RANGE`
- [ ] `regular file EPERM` wenn Mode nicht unterstützt → `-EOPNOTSUPP`

### 4.5 xattr (2+ Failures)

Heute global `-ENODATA` in `dispatch.c:202-206`.

- [ ] Per-fd-Dispatch: FS-Typ-spezifisch
- [ ] `setxattr`/`getxattr`/`listxattr`/`removexattr`: `-EOPNOTSUPP` statt `-ENODATA` wenn FS nicht unterstützt
- [ ] `flistxattr` zero-size return (benötigte Buffer-Größe)
- [ ] tmpfs: in-memory xattr (optional, minimaler Pfad: `-EOPNOTSUPP` für alle)

### 4.6 eventfd EFD_SEMAPHORE (5 Failures)

- [ ] Eigener Read-Pfad: bei `EFD_SEMAPHORE` read liefert `1`, decrementiert counter um 1
- [ ] write-Overflow: `val > UINT64_MAX - counter - 1` → `-EINVAL` (nicht `-EAGAIN`, das ist non-block-Fall)

### 4.7 `chroot` (4 Failures)

- [ ] `fs/open.c:ksys_chroot`-Equivalent
- [ ] Pfad-Lookup, Typ-Prüfung (muss Directory sein)
- [ ] `process.root` Feld setzen, alle Pfad-Resolves berücksichtigen
- [ ] ENOENT / ENOTDIR / ELOOP / ENAMETOOLONG korrekt

### 4.8 `acct` (4 Failures)

- [ ] Pfad-Lookup, Typ-Validierung
- [ ] EFAULT bei bad ptr, EISDIR bei directory, ENOENT, ENOTDIR
- [ ] Danach `-ENOSYS` (Accounting selbst nicht implementiert — OK)
- [ ] Bei regular file → `-EPERM` (nicht erlaubter Pfad)

### 4.9 `fadvise64` Validierung

Heute `return 0` (Stub). Kein Impact auf Verhalten, aber Fehlerpfade müssen korrekt sein.

- [ ] fd-Lookup → `-EBADF`
- [ ] advice-Range-Check → `-EINVAL`
- [ ] Pipe/Socket → `-ESPIPE`
- [ ] Dann: `return 0` (Hints sind optional)

---

## Phase 5 — Loopback-Vollendung

Commit `7b85a5f` hat Loopback begonnen, Tests failen weiterhin. Audit benötigt.

- [ ] `net/lo.c` auf aktuelle `netif`-API portieren
- [ ] 127.0.0.0/8 Routing: direkter `net_rx`, kein Gateway-Check
- [ ] `connect` lokaler listener: TCP-Statemachine durch Loopback
- [ ] `connect` auf unbekannten Port → `-ECONNREFUSED` (Linux-konform)
- [ ] `connect` auf bereits verbundenen Socket → `-EISCONN`
- [ ] **Accept-Deadline entfernen** (`do_accept` aktuell 1s-Timeout, `NET_TCP_TIMEOUT_MS` — Linux hat keins); blocking accept nutzt Socket-Wait-Queue aus Phase 6.5
- [ ] Tests: `net/accept-loopback`, `ltp/accept02-loopback`, `ltp/connect*`, `net/tcp_hash_multi`

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

### 6.5 Socket-Readiness-Wakeup

**Wurzel (Netz-Audit, Stand `3d26a2e`):** `tcp_input` (`net/tcp.c:747`) postet Events nur an `c->wait_thread` — dieser Slot wird aber nur von blocking `do_connect`/`do_recv` gesetzt. Für `O_NONBLOCK + poll(POLLOUT|POLLIN)` weiß der Input-Pfad nichts vom Poller → `do_poll` (`sys_event.c:97`) schläft bis zur Deadline, während SYN-ACK/Daten längst da sind.

Zweite Wurzel: `net_arp_resolve` busy-waits 3s ohne Signal-Check (`arp.c:190`). Linux: Paket queuen, async auflösen.

Betroffene Tests: `net/nonblock-connect`, `-read`, `-write` (stabil rot), `ltp/accept02-loopback`. Alle vier zusammen durch 6.5 lösbar, da gemeinsame Infrastruktur.

Linux-Vorbild: `struct sock.sk_wq` + `sock_def_readable`/`sock_def_write_space` + `poll_wait()` im `file_operations.poll`-Hook + `wait_event_interruptible_timeout`.

- [ ] Per-Socket Wait-Queue (`sk_wq`-Äquivalent), Multi-Waiter-Liste, statt Single-Slot `wait_thread`
- [ ] `tcp_input`/`udp_input`/Loopback-RX wecken **alle** registrierten Poller (`sock_wake_all`)
- [ ] `do_poll` registriert sich via `sock_poll_wait_register(sk, t)` **vor** Readiness-Check, deregistriert vor Return
- [ ] Accept-Pfad nutzt dieselbe Queue (ersetzt `q_tcp_wait_thread` globalen Slot in `net.c:55`)
- [ ] `net_arp_resolve` non-blocking: bei Miss Paket in Queue + TTL, `-EAGAIN` zurück
- [ ] TX-Pfad identisch: Completion weckt wartende Writer statt Busy-Wait
- [ ] Test-Varianz-Check: 5× `make test-hw` muss identische PASS/FAIL-Liste produzieren

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
