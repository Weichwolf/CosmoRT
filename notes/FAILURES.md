# ktest 2076/193 — Failure Analysis

Jeder Failure mit Root Cause und Resolution.

## A. VFS Path-Resolution — vfs_lookup gibt ENOENT statt spezifischen Fehler

Cause: `vfs_lookup` prueft nicht ob Pfad-Component ein Directory ist, limitiert
Symlink-Tiefe nicht auf ELOOP, prueft Namenlaenge nicht.
Resolution: `vfs_lookup_nofollow` nach Linux `fs/namei.c:link_path_walk` umschreiben.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 1 | access ENOTDIR | -2 | -20 | vfs_lookup: Non-Dir Component → ENOTDIR |
| 2 | access ELOOP | -2 | -40 | vfs_lookup: Symlink depth >40 → ELOOP |
| 3 | chmod through file ENOTDIR | -2 | -20 | (gleich) |
| 4 | chmod ELOOP | -2 | -40 | (gleich) |
| 5 | chown through file ENOTDIR | -2 | -20 | (gleich) |
| 6 | chown ELOOP | -2 | -40 | (gleich) |
| 7 | chdir ENAMETOOLONG | -2 | -36 | vfs_lookup: Component >255 → ENAMETOOLONG |
| 8 | creat ENAMETOOLONG | 3 (fd) | -36 | (gleich) |
| 9 | creat ENOENT | 3 (fd) | -2 | creat in non-existent dir erstellt statt ENOENT |
| 10 | utime ENOTDIR | -2 | -20 | (gleich) |
| 11 | fchmodat empty ENOENT | 0 | -2 | fchmodat leerer Pfad nicht geprueft |
| 12 | fchownat ELOOP | -2 | -40 | (gleich) |
| 13 | ENAMETOOLONG | -2 | -36 | fchmodat: Namenlaenge |
| 14 | ENOTDIR | -2 | -20 | fchownat/fchmodat: Non-Dir Component |
| 15 | circular ELOOP | 0 | -40 | Symlink-Zirkel nicht erkannt |
| 16 | nesting > 5 fails | | | Symlink-Nesting falsch limitiert |
| 17 | slash paths all resolve to / | 4 | 0 | chroot slash-Pfad Resolution |

## B. FD-Typ-Dispatch — EBADF statt ENOTSOCK/EOPNOTSUPP

Cause: `sock_from_fd()` gibt NULL bei Non-Socket → Caller nimmt EBADF an.
Kein zweistufiger Check (fd gueltig? → typ korrekt?).
Resolution: In socket.c: erst `fd_lookup` (EBADF), dann type==FD_SOCKET (ENOTSOCK).

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 18 | accept file ENOTSOCK | -9 | -88 | sock_from_fd zweistufig |
| 19 | accept dir ENOTSOCK | -9 | -88 | (gleich) |
| 20 | accept pipe ENOTSOCK | -9 | -88 | (gleich) |
| 21 | accept epoll ENOTSOCK | -9 | -88 | (gleich) |
| 22 | accept eventfd ENOTSOCK | -9 | -88 | (gleich) |
| 23 | accept timerfd ENOTSOCK | -9 | -88 | (gleich) |
| 24 | accept inotify ENOTSOCK | -9 | -88 | (gleich) |
| 25 | accept memfd ENOTSOCK | -9 | -88 | (gleich) |
| 26 | accept ENOTSOCK | -9 | -88 | (gleich, test/libc) |
| 27 | bind file ENOTSOCK | -9 | -88 | (gleich) |
| 28 | bind pipe ENOTSOCK | -9 | -88 | (gleich) |
| 29 | connect file ENOTSOCK | -9 | -88 | (gleich) |
| 30 | accept UDP EOPNOTSUPP | -22 | -95 | accept auf SOCK_DGRAM → EOPNOTSUPP statt EINVAL |

## C. Loopback / Netzwerk — TIMEOUT bei 127.0.0.1

Cause: Kein Loopback-Device. Gateway-Check blockiert Connections bei `net_gw_ip==0`.
Resolution: Loopback-Device implementieren (Pakete an 127.0.0.0/8 direkt an net_rx).
Gateway-Check fuer Loopback-Adressen ueberspringen.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 31 | net/accept-loopback | TIMEOUT | accept fd | Loopback-Device |
| 32 | net/accept4-flags | TIMEOUT | O_NONBLOCK | (gleich) |
| 33 | ltp/accept02-loopback | TIMEOUT | accept fd | (gleich) |
| 34 | ltp/accept4-noflags | TIMEOUT | accept fd | (gleich) |
| 35 | ltp/accept4-both | TIMEOUT | accept fd | (gleich) |
| 36 | ltp/accept4-nonblock | TIMEOUT | accept fd | (gleich) |
| 37 | ltp/accept4-cloexec | TIMEOUT | accept fd | (gleich) |
| 38 | ltp/connect01-econnrefused | TIMEOUT | -ECONNREFUSED | (gleich) |
| 39 | ltp/connect02 | TIMEOUT | data transfer | (gleich) |
| 40 | ltp/connect-eisconn | TIMEOUT | -EISCONN | (gleich) |
| 41 | net/tcp_hash_multi | TIMEOUT | | TCP auf Loopback |

## D. epoll Event-Delivery — epoll_wait gibt 0 statt Events

Cause: Kein Push-Notification von Datenquellen an epoll. `fd_poll_readiness` pollt
einmalig, Datenquellen (pipe_write, socket) rufen `epoll_wake_all` nicht auf.
Resolution: Jede Datenquelle muss bei State-Aenderung `epoll_wake_all` aufrufen.
Langfristig: file_operations mit poll-Callback (Linux-Modell).

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 42 | epoll_wait EPOLLIN returns 1 | 0 | 1 | pipe_write → epoll_wake_all |
| 43 | epoll_wait after ADD returns >= 1 | 0 | >=1 | Initial readiness bei ADD pruefen |
| 44 | ET first wait returns 1 | 0 | 1 | EPOLLET: Edge-Triggered Wakeup |
| 45 | oneshot first wait returns 1 | 0 | 1 | EPOLLONESHOT Delivery |
| 46 | epoll_pwait data ready returns 1 | 0 | 1 | epoll_pwait Sigmask + Readiness |
| 47 | epoll_pwait with sigmask returns 1 | 0 | 1 | (gleich) |
| 48 | ltp/epoll_wait01-in | TIMEOUT | EPOLLIN | pipe→epoll Wakeup |
| 49 | ltp/epoll_wait07 | TIMEOUT | EPOLLONESHOT | (gleich) |
| 50 | ltp/epoll_ctl01 | TIMEOUT | MOD/DEL | epoll_ctl mit pipe |
| 51 | ltp/epoll_pwait01 | TIMEOUT | | epoll_pwait |
| 52 | ltp/epoll_pwait02 | TIMEOUT | | (gleich) |
| 53 | ltp/epoll_pwait05 | TIMEOUT | | (gleich) |
| 54 | fd==epfd EINVAL | 0 | -22 | epoll_ctl: epfd als Target ablehnen |
| 55 | epoll_wait on pipe EINVAL | -9 | -22 | epoll_wait auf Non-Epoll-FD → EINVAL |

## E. fadvise64 — Stub return 0

Cause: `do_fadvise64` ist `return 0`. Keine Argument-Validierung.
Resolution: Implementieren nach Linux `mm/fadvise.c`: fd-Lookup, Typ-Check, Advice-Validierung.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 56-61 | fadvise64 bad fd (6x) | 0 | -9 | fd_lookup → EBADF |
| 62-68 | fadvise64 invalid advice (7x) | 0 | -22 | advice range check → EINVAL |
| 69-74 | fadvise64 pipe ESPIPE (6x) | 0 | -29 | Pipe/Socket → ESPIPE |

## F. chmod/fchmod Mode-Bits — Mode wird nicht gesetzt

Cause: `do_chmod`/`do_fchmod` aktualisiert `vfs_node.mode` nicht, oder ramfs
ueberschreibt den Wert. creat ignoriert mode-Argument.
Resolution: mode-Feld korrekt in VFS-Operationen durchreichen.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 75 | mode 0000 | 493 | 0 | chmod setzt mode nicht |
| 76 | mode 0444 | 493 | 292 | (gleich) |
| 77 | mode 0644 | 493 | 420 | (gleich) |
| 78 | mode 0700 | 493 | 448 | (gleich) |
| 79-80 | mode bits (2x) | 384/448 | 0 | fchmod setzt mode nicht |
| 81-82 | suid/sgid cleared (2x) | 3072 | 0 | chown muss SUID/SGID loeschen |
| 83 | read on creat fd EBADF | 0 | -9 | creat-fd ist O_WRONLY → read EBADF |
| 84 | EACCES | -8 | -13 | access X_OK auf Non-Exec → EACCES |

## G. chown/fchown uid/gid — Werte nicht gespeichert

Cause: `vfs_node` hat keine uid/gid Felder. chown/fchown ist No-Op.
Resolution: uid/gid in `vfs_node` speichern, in stat zurueckgeben.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 85-86 | uid (2x) | 0 | 1000 | uid in vfs_node speichern |
| 87-88 | gid (2x) | 0 | 1000 | gid in vfs_node speichern |
| 89 | uid | 0 | 700 | (gleich) |
| 90 | uid | 0 | 702 | (gleich) |
| 91 | uid nop | 0 | 702 | (gleich) |
| 92 | uid unchanged | 0 | 702 | (gleich) |
| 93 | gid | 0 | 701 | (gleich) |
| 94 | gid | 0 | 704 | (gleich) |
| 95 | gid nop | 0 | 704 | (gleich) |
| 96 | gid unchanged | 0 | 701 | (gleich) |
| 97 | link uid | 0 | 1000 | hard link erbt uid |
| 98 | link gid | 0 | 1000 | hard link erbt gid |

## H. acct — Stub return 0

Cause: `do_acct` ist `return 0`. Keine Pfad-Validierung.
Resolution: Pfad-Pruefung implementieren. Nicht-vorhandene Datei → ENOENT,
Directory → EISDIR, Non-regular → EPERM, EFAULT. Accounting selbst: ENOSYS ok.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 99 | acct(".") EISDIR | 0 | -21 | Pfad-Typ pruefen |
| 100 | acct ENOENT | 0 | -2 | Pfad-Existenz pruefen |
| 101 | acct ENOTDIR | 0 | -20 | Pfad-Component pruefen |
| 102 | acct EFAULT | 0 | -14 | user_ok Pruefung |

## I. capget/capset — Stub return -1

Cause: `do_capget`/`do_capset` geben -1 zurueck. Linux: parst Version-Header,
liest/schreibt Capability-Sets.
Resolution: Version-Header-Parsing + Single-User: alle Capabilities gesetzt.
Vorbild: `kernel/capability.c`.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 103 | capget v1 | -1 | 0 | capget implementieren |
| 104 | capget v2 | -1 | 0 | (gleich) |
| 105 | capget v3 | -1 | 0 | (gleich) |
| 106 | capget before set | -1 | 0 | (gleich) |
| 107 | capset same caps | -1 | 0 | capset implementieren |
| 108 | bad version EINVAL (2x) | -1 | -22 | Version-Pruefung |
| 109 | kernel sets v3 | 0 | 537396514 | capget setzt preferred Version |
| 110 | nonexistent pid ESRCH | -1 | -3 | PID-Lookup |
| 111 | pid=-1 EINVAL | -1 | -22 | Argument-Validierung |
| 112 | NULL data EFAULT | -1 | -14 | user_ok Pruefung |

## J. clone/clone3 Flag-Validierung

Cause: `do_clone`/`do_clone3` validiert Flag-Kombinationen nicht.
Resolution: Flag-Checks aus Linux `kernel/fork.c:copy_process` uebernehmen.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 113-114 | clone NULL stack EINVAL | 0/586 | -22 | CLONE_VM ohne Stack → EINVAL |
| 115-116 | CLONE_SIGHAND w/o CLONE_VM | 0/567 | -22 | Flag-Abhaengigkeit pruefen |
| 117 | CLONE_THREAD w/o CLONE_SIGHAND | 0 | -22 | (gleich) |
| 118-119 | no stack but stack_size | 0/560 | -22 | clone3: Stack-Args pruefen |
| 120 | short size EINVAL | 569 | -22 | clone3: size < min → EINVAL |

## K. chroot — Stub return 0

Cause: `do_chroot` ist `return 0`. Pfad wird weder validiert noch Root geaendert.
Resolution: Pfad-Lookup + `process.root` setzen. Vorbild: `fs/open.c:ksys_chroot`.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 121 | chroot ENOENT | 0 | -2 | Pfad-Existenz pruefen |
| 122 | chroot ENOTDIR | 0 | -20 | Pfad-Typ pruefen |
| 123 | chroot ELOOP | 0 | -40 | Symlink-Limit |
| 124 | chroot ENAMETOOLONG | 0 | -36 | Namenlaenge pruefen |

## L. clock_settime/adjtimex — Fehlende Validierung

Cause: clock_settime akzeptiert ungueltige Argumente. adjtimex prueft tick/mode/EFAULT nicht.
Resolution: Argument-Validierung nach Linux `kernel/time/posix-timers.c`, `kernel/time/ntp.c`.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 125 | CLOCK_MONOTONIC EINVAL | 0 | -22 | Monotonic clock nicht setzbar |
| 126 | CLOCK_BOOTTIME EINVAL | 0 | -22 | Boottime clock nicht setzbar |
| 127 | invalid clock EINVAL (3x) | 0 | -22 | Unbekannte clock_id ablehnen |
| 128 | invalid clock -1 | 0 | -22 | (gleich) |
| 129 | tv_sec=-1 EINVAL | 0 | -22 | Negative Sekunden ablehnen |
| 130 | tv_nsec=-1 EINVAL | 0 | -22 | Negative Nanosekunden |
| 131 | tv_nsec=1e9 EINVAL | 0 | -22 | nsec >= 1e9 |
| 132 | tv_nsec>1e9 EINVAL | 0 | -22 | (gleich) |
| 133 | negative tv_nsec EINVAL | 0 | -22 | clock_nanosleep |
| 134 | NULL timespec EFAULT | 0 | -14 | NULL-Pointer Pruefung |
| 135-136 | NULL timex EFAULT (2x) | 0 | -14 | adjtimex NULL Pruefung |
| 137 | invalid mode EINVAL | 0 | -22 | adjtimex ungueltige Mode |
| 138 | tick too high EINVAL | 0 | -22 | adjtimex tick out of range |
| 139 | tick too low EINVAL | 0 | -22 | (gleich) |
| 140 | time advanced >= 9s | 0 | >=9 | clock_settime aendert Zeit nicht |

## M. alarm — Runner-Interference

Cause: Test-Runner setzt alarm(5) im Child. Alarm-Tests ueberschreiben mit alarm(N),
alarm(1) gibt dann 5 statt 0 zurueck (Restwert des Runner-Alarms).
Resolution: Kein Kernel-Bug. Runner darf alarm nicht im Child setzen (parent-side
Timeout verwenden), oder alarm-Tests muessen Runner-Alarm beruecksichtigen.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 141 | alarm(1) old=0 | 5 | 0 | Runner-Timeout parent-seitig |
| 142 | alarm(100) returns 0 | 5 | 0 | (gleich) |
| 143 | alarm(100) first call returns 0 | 5 | 0 | (gleich) |
| 144 | alarm(1) returns 0 | 5 | 0 | (gleich) |

## N. Signal-Delivery bei Blocking

Cause: Teilweise gefixt. nanosleep gibt -EINTR zurueck aber Handler laeuft erst
danach in check_pending_signals. Der Test prueft alarms_received VOR check_pending.
Resolution: Signal-Handler muss VOR dem Syscall-Return laufen. Syscall-Restart-Pfad
muss Signal delivery triggern bevor -EINTR an Userspace geht.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 145 | parent got SIGALRM | 0 | 1 | Handler vor -EINTR ausfuehren |
| 146 | clock_nanosleep mono | -4 | 0 | nanosleep EINTR bei CLOCK_MONOTONIC |

## O. flock — Nicht implementiert

Cause: `do_flock` ist Stub. Keine Lock-Semantik, keine EBADF/EINVAL Pruefung.
Resolution: Implementieren nach Linux `fs/locks.c`. Advisory Locking auf vfs_node.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 147 | flock(-1) EBADF | 0 | -9 | fd-Validierung |
| 148 | SH\|EX EINVAL | 0 | -22 | Mutex-Flags pruefen |
| 149 | LOCK_NB alone EINVAL | 0 | -22 | (gleich) |
| 150 | child SH ok, EX blocked | | | Lock-Semantik |
| 151 | child SH+EX both blocked | | | (gleich) |
| 152 | child got EWOULDBLOCK | | | LOCK_NB + Conflict → EWOULDBLOCK |
| 153 | fd2 EX denied | 0 | -11 | (gleich) |

## P. fcntl Locks — Lock-Semantik unvollstaendig

Cause: F_SETLK auf Pipe gibt 0 statt EINVAL. Ungueltiger Lock-Typ nicht abgelehnt.
F_SETLKW blockiert nicht bis Lock frei. Partial unlock nicht korrekt.
Resolution: Lock-Validierung und Advisory-Lock-Implementierung nach Linux `fs/locks.c`.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 154 | F_SETLK pipe read EINVAL | 0 | -22 | Pipe → EINVAL |
| 155 | F_SETLK pipe write EINVAL | 0 | -22 | (gleich) |
| 156 | F_SETLK bad type EINVAL | 0 | -22 | Lock-Typ validieren |
| 157 | partial unlock correct | 1 | 0 | Partial unlock |
| 158 | ltp/fcntl15 | TIMEOUT | | F_SETLKW blockiert nicht/weckt nicht |

## Q. eventfd — write UINT64_MAX nicht validiert

Cause: eventfd_write akzeptiert UINT64_MAX. Linux: EINVAL weil Overflow.
Resolution: write-Wert >= UINT64_MAX → EINVAL. Vorbild: `fs/eventfd.c`.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 159 | write UINT64_MAX EINVAL | 8 | -22 | Overflow-Check |
| 160 | sem value 1 (3x) | 3 | 1 | Semaphore-Modus |
| 161 | sem read 2 | -11 | 8 | read nach sem_post |
| 162 | sem read 3 | -11 | 8 | (gleich) |

## R. epoll_create — Fehlende Validierung

Cause: epoll_create akzeptiert ungueltige Argumente (0, -1, bad flags).
Resolution: size <= 0 → EINVAL, ungueltige flags → EINVAL.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 163 | epoll_create(0) EINVAL | 3 | -22 | size-Check |
| 164 | epoll_create(-1) EINVAL | 3 | -22 | (gleich) |
| 165 | epoll_create1(-1) EINVAL | 3 | -22 | flags-Check |
| 166 | epoll_create1(CLOEXEC+1) | 3 | -22 | (gleich) |

## S. execveat — Return -ENOSYS statt korrekte Fehler

Cause: `do_execveat` ist nicht implementiert, gibt -ENOSYS (-38) zurueck.
Resolution: Implementieren nach Linux `fs/exec.c:do_execveat_common`.

| # | Failed Test | Got | Expected | Resolution |
|---|-------------|-----|----------|------------|
| 167 | execveat bad dirfd EBADF | -38 | -9 | Implementieren |
| 168 | execveat invalid flags | -38 | -22 | (gleich) |
| 169 | execveat notdir ENOTDIR | -38 | -20 | (gleich) |
| 170 | execveat symlink ELOOP | -38 | -40 | (gleich) |

## T. Sonstige Einzelfehler

| # | Failed Test | Got | Expected | Cause | Resolution |
|---|-------------|-----|----------|-------|------------|
| 171 | close_range bad flags | 0 | -22 | Flags nicht validiert | Flag-Check in do_close_range |
| 172 | fdatasync(pipe) EINVAL | 0 | -22 | Pipe nicht geprueft | FD-Typ pruefen in do_fdatasync |
| 173 | dup3 invalid flags | 52 | -22 | Flags nicht validiert | Flag-Check in do_dup3 |
| 174 | fchmodat2 EINVAL | 0 | -22 | Flags nicht validiert | Flag-Check in do_fchmodat2 |
| 175 | fchownat EINVAL | 0 | -22 | Flags nicht validiert | Flag-Check in do_fchownat |
| 176 | double bind EINVAL | 0 | -22 | Doppeltes bind nicht erkannt | already-bound Check in do_bind |
| 177 | invalid flags EINVAL | 0 | -22 | clone3/faccessat2 Flags | Flag-Validierung |
| 178 | atime 64-bit | 0 | 4294967296 | 32-bit Truncation | vfs_node timestamps auf int64_t |
| 179 | mtime 64-bit | 0 | 4294967296 | (gleich) | (gleich) |
| 180 | chmod empty ENOENT | 0 | -2 | Leerer Pfad nicht geprueft | Pfad-Validierung |
| 181 | FD_CLOEXEC set | | | dup3 O_CLOEXEC wird nicht gesetzt | F_GETFD nach dup3 |
| 182 | fd still open | | | close_range schliesst nicht | close_range Implementierung |
| 183 | data matches | | | copy_file_range Daten falsch | copy_file_range Implementierung |
| 184 | fallocate KEEP_SIZE | -95 | 0 | FALLOC_FL_KEEP_SIZE → ENOSYS | fallocate Flags implementieren |
| 185 | regular file EPERM | 0 | -1 | acct auf regular file | acct Implementierung |
| 186 | xattr flistxattr zero-size | | | flistxattr nicht implementiert | xattr Subsystem |
| 187 | child exit code | 2 | 0 | clone child Exit-Code falsch | clone Implementierung |
| 188 | child terminated by signal | | | meltdown Test: kein SIGSEGV | Kernel-Memory Protection |
| 189 | child exited normally | | | stack_clash: kein Guard Page Hit | Stack Guard Page |
| 190 | EBADF rdonly | 0 | -9 | creat-fd read auf write-only | O_WRONLY Check bei read |
| 191 | read /proc/stat | | | /proc/stat Format unvollstaendig | procfs: cpu Zeilen |
| 192 | access X_OK EACCES | 0 | -13 | Execute-Bit nicht geprueft | access: mode & X_OK vs i_mode |
| 193 | mono: woke at or after target | | | clock_nanosleep Timing | TSC/Timer Praezision |
