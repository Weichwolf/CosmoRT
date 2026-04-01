# CosmoRT — TODO

Stand: ktest 2090/182, musl 452/20, LTP 11/87

## ktest Failures (193) — nach Root Cause

### B. FD-Typ-Dispatch — 13 Failures

sock_from_fd gibt NULL bei Non-Socket → EBADF statt ENOTSOCK.
Fix: zweistufig — erst fd_lookup (EBADF), dann type-check (ENOTSOCK).

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 18 | accept file ENOTSOCK | -9 | -88 | Kein FD-Typ-Check |
| 19 | accept dir ENOTSOCK | -9 | -88 | (gleich) |
| 20 | accept pipe ENOTSOCK | -9 | -88 | (gleich) |
| 21 | accept epoll ENOTSOCK | -9 | -88 | (gleich) |
| 22 | accept eventfd ENOTSOCK | -9 | -88 | (gleich) |
| 23 | accept timerfd ENOTSOCK | -9 | -88 | (gleich) |
| 24 | accept inotify ENOTSOCK | -9 | -88 | (gleich) |
| 25 | accept memfd ENOTSOCK | -9 | -88 | (gleich) |
| 26 | accept ENOTSOCK (libc) | -9 | -88 | (gleich) |
| 27 | bind file ENOTSOCK | -9 | -88 | (gleich) |
| 28 | bind pipe ENOTSOCK | -9 | -88 | (gleich) |
| 29 | connect file ENOTSOCK | -9 | -88 | (gleich) |
| 30 | accept UDP EOPNOTSUPP | -22 | -95 | SOCK_DGRAM nicht EOPNOTSUPP |

- [ ] sock_from_fd → fd_lookup + type==FD_SOCKET
- [ ] accept auf SOCK_DGRAM → EOPNOTSUPP

### C. Loopback-Netzwerk — 11 Failures (TIMEOUT)

Kein Loopback-Device. Gateway-Check blockiert bei net_gw_ip==0.
Loopback: Pakete an 127.0.0.0/8 direkt an net_rx. Vorbild: Linux `drivers/net/loopback.c`.

| # | Test | Cause |
|---|------|-------|
| 31 | net/accept-loopback | Kein Loopback |
| 32 | net/accept4-flags | (gleich) |
| 33 | ltp/accept02-loopback | (gleich) |
| 34 | ltp/accept4-noflags | (gleich) |
| 35 | ltp/accept4-both | (gleich) |
| 36 | ltp/accept4-nonblock | (gleich) |
| 37 | ltp/accept4-cloexec | (gleich) |
| 38 | ltp/connect01-econnrefused | (gleich) |
| 39 | ltp/connect02 | (gleich) |
| 40 | ltp/connect-eisconn | (gleich) |
| 41 | net/tcp_hash_multi | (gleich) |

- [ ] Loopback-Device implementieren
- [ ] Gateway-Check fuer 127.0.0.0/8 ueberspringen

### D. epoll Event-Delivery — 14 Failures

fd_poll_readiness pollt einmalig, Datenquellen rufen epoll_wake_all nicht auf.
Linux: jeder struct file hat poll-Callback, Datenquelle ruft wake_up bei Events.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 42 | epoll_wait EPOLLIN returns 1 | 0 | 1 | pipe→epoll Wakeup fehlt |
| 43 | epoll_wait after ADD returns >=1 | 0 | >=1 | Initial readiness nicht geprueft |
| 44 | ET first wait returns 1 | 0 | 1 | EPOLLET Wakeup fehlt |
| 45 | oneshot first wait returns 1 | 0 | 1 | EPOLLONESHOT Delivery |
| 46 | epoll_pwait data ready returns 1 | 0 | 1 | epoll_pwait Readiness |
| 47 | epoll_pwait with sigmask returns 1 | 0 | 1 | (gleich) |
| 48 | ltp/epoll_wait01-in | TIMEOUT | | pipe→epoll Wakeup |
| 49 | ltp/epoll_wait07 | TIMEOUT | | EPOLLONESHOT |
| 50 | ltp/epoll_ctl01 | TIMEOUT | | epoll_ctl mit pipe |
| 51 | ltp/epoll_pwait01 | TIMEOUT | | epoll_pwait |
| 52 | ltp/epoll_pwait02 | TIMEOUT | | (gleich) |
| 53 | ltp/epoll_pwait05 | TIMEOUT | | (gleich) |
| 54 | fd==epfd EINVAL | 0 | -22 | epfd als Target nicht abgelehnt |
| 55 | epoll_wait on pipe EINVAL | -9 | -22 | Non-Epoll-FD nicht erkannt |

- [ ] pipe_write/pipe_close → epoll_wake_all
- [ ] socket accept/connect/recv → epoll_wake_all
- [ ] epoll_ctl ADD: Initial readiness pruefen
- [ ] epoll_ctl: fd==epfd → EINVAL
- [ ] epoll_wait: Non-Epoll-FD → EINVAL statt EBADF

### E. fadvise64 — Stub return 0 — 19 Failures

do_fadvise64 ist `return 0`. Implementieren nach Linux `mm/fadvise.c`.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 56-61 | fadvise64 bad fd (6x) | 0 | -9 | Kein fd-Lookup |
| 62-68 | fadvise64 invalid advice (7x) | 0 | -22 | Kein advice-Check |
| 69-74 | fadvise64 pipe ESPIPE (6x) | 0 | -29 | Kein Typ-Check |

- [ ] fadvise64: fd-Lookup, advice-Validierung, Pipe/Socket → ESPIPE

### F. chmod/fchmod/creat Mode-Bits — 10 Failures

Mode-Bits werden nicht korrekt gesetzt. creat ignoriert mode. suid/sgid bei chown nicht geloescht.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 75 | mode 0000 | 493 | 0 | chmod setzt vfs_node.mode nicht |
| 76 | mode 0444 | 493 | 292 | (gleich) |
| 77 | mode 0644 | 493 | 420 | (gleich) |
| 78 | mode 0700 | 493 | 448 | (gleich) |
| 79-80 | mode bits (2x) | 384/448 | 0 | fchmod setzt mode nicht |
| 81-82 | suid/sgid cleared (2x) | 3072 | 0 | chown loescht suid/sgid nicht |
| 83 | read on creat fd EBADF | 0 | -9 | creat-fd read auf O_WRONLY |
| 84 | access X_OK EACCES | 0 | -13 | Execute-Bit nicht geprueft |

- [ ] do_chmod/do_fchmod: vfs_node.mode = new_mode & 07777
- [ ] do_chown: SUID/SGID Bits loeschen (Linux `fs/attr.c:notify_change`)
- [ ] creat: mode-Argument durchreichen
- [ ] read auf O_WRONLY fd → EBADF
- [ ] access X_OK: i_mode & S_IXUSR/S_IXGRP/S_IXOTH pruefen

### G. chown/fchown uid/gid — 14 Failures

vfs_node hat keine uid/gid Felder. chown ist No-Op. Werte muessen gespeichert
und in stat zurueckgegeben werden (auch wenn Single-User keine Permissions prueft).

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 85-86 | uid (2x) | 0 | 1000 | uid nicht gespeichert |
| 87-88 | gid (2x) | 0 | 1000 | gid nicht gespeichert |
| 89-92 | uid 700/702/nop/unchanged | 0 | 700-702 | (gleich) |
| 93-96 | gid 701/704/nop/unchanged | 0 | 701-704 | (gleich) |
| 97-98 | link uid/gid | 0 | 1000 | hard link erbt uid/gid |

- [ ] uid/gid Felder in vfs_node
- [ ] do_chown/do_fchown/do_fchownat: uid/gid setzen
- [ ] do_stat/do_fstat: uid/gid zurueckgeben

### H. acct — Stub return 0 — 4 Failures

do_acct ist `return 0`. Muss Pfad validieren. Accounting selbst darf ENOSYS sein.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 99 | acct(".") EISDIR | 0 | -21 | Kein Pfad-Check |
| 100 | acct ENOENT | 0 | -2 | (gleich) |
| 101 | acct ENOTDIR | 0 | -20 | (gleich) |
| 102 | acct EFAULT | 0 | -14 | Kein user_ok Check |

- [ ] do_acct: EFAULT, Pfad-Lookup, EISDIR, ENOENT, ENOTDIR, dann -ENOSYS

### I. capget/capset — Stub return -1 — 10 Failures

Nicht implementiert. Linux: Version-Header parsen, Capability-Sets lesen/schreiben.
Single-User: alle Capabilities gesetzt. Vorbild: `kernel/capability.c`.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 103-106 | capget v1/v2/v3/before-set | -1 | 0 | Nicht implementiert |
| 107 | capset same caps | -1 | 0 | (gleich) |
| 108 | bad version EINVAL (2x) | -1 | -22 | (gleich) |
| 109 | kernel sets v3 | 0 | 537396514 | (gleich) |
| 110 | nonexistent pid ESRCH | -1 | -3 | (gleich) |
| 111 | pid=-1 EINVAL | -1 | -22 | (gleich) |
| 112 | NULL data EFAULT | -1 | -14 | (gleich) |

- [ ] capget/capset implementieren nach Linux kernel/capability.c

### J. clone/clone3 Flag-Validierung — 8 Failures

Flag-Kombinationen nicht validiert. Linux `kernel/fork.c:copy_process` hat >20 Regeln.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 113-114 | clone NULL stack EINVAL | 0/586 | -22 | CLONE_VM ohne Stack |
| 115-116 | CLONE_SIGHAND w/o CLONE_VM | 0/567 | -22 | Flag-Abhaengigkeit |
| 117 | CLONE_THREAD w/o CLONE_SIGHAND | 0 | -22 | (gleich) |
| 118-119 | no stack but stack_size | 0/560 | -22 | clone3 Stack-Args |
| 120 | short size EINVAL | 569 | -22 | clone3 size < min |

- [ ] clone: Flag-Checks aus Linux kernel/fork.c:copy_process
- [ ] clone3: size-Validierung, Stack-Args-Validierung

### K. chroot — Stub return 0 — 4 Failures

do_chroot ist `return 0`. Muss Pfad-Lookup + process.root setzen.
Vorbild: `fs/open.c:ksys_chroot`.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 121 | chroot ENOENT | 0 | -2 | Nicht implementiert |
| 122 | chroot ENOTDIR | 0 | -20 | (gleich) |
| 123 | chroot ELOOP | 0 | -40 | (gleich) |
| 124 | chroot ENAMETOOLONG | 0 | -36 | (gleich) |

- [ ] do_chroot: Pfad-Lookup, Typ-Pruefung, process.root setzen

### L. clock_settime/adjtimex Validierung — 16 Failures

clock_settime akzeptiert alles. adjtimex prueft tick/mode/EFAULT nicht.
Vorbild: `kernel/time/posix-timers.c`, `kernel/time/ntp.c`.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 125 | CLOCK_MONOTONIC EINVAL | 0 | -22 | Monotonic nicht ablehnbar |
| 126 | CLOCK_BOOTTIME EINVAL | 0 | -22 | (gleich) |
| 127-128 | invalid clock (4x) | 0 | -22 | clock_id nicht validiert |
| 129-132 | tv_sec/nsec invalid (4x) | 0 | -22 | timespec nicht validiert |
| 133 | negative tv_nsec | 0 | -22 | clock_nanosleep |
| 134 | NULL timespec EFAULT | 0 | -14 | NULL nicht geprueft |
| 135-136 | NULL timex EFAULT (2x) | 0 | -14 | (gleich) |
| 137 | invalid mode EINVAL | 0 | -22 | adjtimex mode |
| 138-139 | tick high/low (2x) | 0 | -22 | adjtimex tick range |
| 140 | time advanced >= 9s | 0 | >=9 | clock_settime wirkt nicht |

- [ ] clock_settime: clock_id, timespec validieren, CLOCK_MONOTONIC/BOOTTIME → EINVAL
- [ ] clock_settime: Zeit tatsaechlich aendern
- [ ] adjtimex: EFAULT, mode, tick-range validieren

### M. alarm Runner-Interference — 4 Failures

Test-Runner setzt alarm(5) im Child. Alarm-Tests sehen Restwert.
Kein Kernel-Bug — Runner-Timeout muss parent-seitig werden.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 141 | alarm(1) old=0 | 5 | 0 | Runner-alarm im Child |
| 142-144 | alarm returns 0 (3x) | 5 | 0 | (gleich) |

- [ ] Test-Runner: Timeout parent-seitig (fork watchdog) statt alarm im Child

### N. Signal-Delivery bei Blocking — 2 Failures

nanosleep gibt -EINTR bevor Handler laeuft. Handler laeuft erst in
check_pending_signals NACH Syscall-Return.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 145 | parent got SIGALRM | 0 | 1 | Handler nach -EINTR |
| 146 | clock_nanosleep mono | -4 | 0 | EINTR bei CLOCK_MONOTONIC |

- [ ] Signal-Handler VOR Syscall-Return ausfuehren wenn -EINTR

### O. flock — Stub — 7 Failures

do_flock ist Stub. Keine Lock-Semantik. Implementieren nach Linux `fs/locks.c`.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 147 | flock(-1) EBADF | 0 | -9 | Nicht implementiert |
| 148 | SH\|EX EINVAL | 0 | -22 | (gleich) |
| 149 | LOCK_NB alone EINVAL | 0 | -22 | (gleich) |
| 150 | child SH ok, EX blocked | | | (gleich) |
| 151 | child SH+EX both blocked | | | (gleich) |
| 152 | child got EWOULDBLOCK | | | (gleich) |
| 153 | fd2 EX denied | 0 | -11 | (gleich) |

- [ ] flock implementieren nach Linux fs/locks.c

### P. fcntl Locks — 5 Failures

F_SETLK auf Pipe akzeptiert, bad lock type akzeptiert, partial unlock falsch.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 154-155 | F_SETLK pipe EINVAL (2x) | 0 | -22 | Pipe nicht abgelehnt |
| 156 | F_SETLK bad type EINVAL | 0 | -22 | Lock-Typ nicht validiert |
| 157 | partial unlock correct | 1 | 0 | Partial unlock falsch |
| 158 | ltp/fcntl15 TIMEOUT | | | F_SETLKW weckt nicht |

- [ ] F_SETLK: Pipe → EINVAL, Lock-Typ validieren
- [ ] Partial unlock korrekt implementieren
- [ ] F_SETLKW: Blocking + Signal-Wakeup

### Q. eventfd Semantik — 5 Failures

write UINT64_MAX nicht abgelehnt, EFD_SEMAPHORE Modus nicht implementiert.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 159 | write UINT64_MAX EINVAL | 8 | -22 | Overflow nicht geprueft |
| 160 | sem value 1 (3x) | 3 | 1 | EFD_SEMAPHORE fehlt |
| 161-162 | sem read (2x) | -11 | 8 | (gleich) |

- [ ] eventfd write: val >= UINT64_MAX - counter → EINVAL/EAGAIN
- [ ] EFD_SEMAPHORE: read gibt 1 statt counter, decrementiert um 1

### R. epoll_create Validierung — 4 Failures

Ungueltige Argumente nicht abgelehnt.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 163-164 | epoll_create(0/-1) | 3 | -22 | size <= 0 akzeptiert |
| 165-166 | epoll_create1 bad flags | 3 | -22 | flags nicht validiert |

- [ ] epoll_create: size <= 0 → EINVAL
- [ ] epoll_create1: unbekannte flags → EINVAL

### S. execveat — Return -ENOSYS — 4 Failures

Nicht implementiert. Vorbild: `fs/exec.c:do_execveat_common`.

| # | Test | Got | Expected | Cause |
|---|------|-----|----------|-------|
| 167 | execveat bad dirfd EBADF | -38 | -9 | Nicht implementiert |
| 168 | execveat invalid flags | -38 | -22 | (gleich) |
| 169 | execveat notdir ENOTDIR | -38 | -20 | (gleich) |
| 170 | execveat symlink ELOOP | -38 | -40 | (gleich) |

- [ ] execveat implementieren (dirfd-relative execve)

### T. Einzelfehler — 23 Failures

| # | Test | Got | Expected | Cause | Resolution |
|---|------|-----|----------|-------|------------|
| 171 | close_range bad flags | 0 | -22 | Flags nicht validiert | Flag-Check |
| 172 | fdatasync(pipe) | 0 | -22 | Pipe akzeptiert | FD-Typ pruefen |
| 173 | dup3 invalid flags | 52 | -22 | Flags nicht validiert | Flag-Check |
| 174 | fchmodat2 EINVAL | 0 | -22 | Flags nicht validiert | Flag-Check |
| 175 | fchownat EINVAL | 0 | -22 | Flags nicht validiert | Flag-Check |
| 176 | double bind EINVAL | 0 | -22 | Doppel-Bind akzeptiert | already-bound Check |
| 177 | invalid flags EINVAL | 0 | -22 | Flags nicht validiert | Flag-Check |
| 178-179 | atime/mtime 64-bit | 0 | 4294967296 | 32-bit Truncation | int64_t timestamps |
| 180 | chmod empty ENOENT | 0 | -2 | Leerer Pfad akzeptiert | Pfad-Validierung |
| 181 | FD_CLOEXEC set | | | dup3 O_CLOEXEC ignoriert | dup3 flags anwenden |
| 182 | fd still open | | | close_range wirkt nicht | close_range implementieren |
| 183 | data matches | | | copy_file_range Daten falsch | copy_file_range fixen |
| 184 | fallocate KEEP_SIZE | -95 | 0 | KEEP_SIZE → ENOSYS | fallocate Flags |
| 185 | regular file EPERM | 0 | -1 | acct auf regular file | acct Typ-Check |
| 186 | flistxattr zero-size | | | flistxattr fehlt | xattr Subsystem |
| 187 | child exit code | 2 | 0 | clone child Exit falsch | clone Exit-Path |
| 188 | child terminated by signal | | | Kein SIGSEGV bei Kernel-Read | Kernel-Memory Protection |
| 189 | child exited normally | | | Kein Guard Page Hit | Stack Guard Page |
| 190 | EBADF rdonly | 0 | -9 | read auf O_WRONLY | O_WRONLY Check bei read |
| 191 | read /proc/stat | | | /proc/stat unvollstaendig | procfs cpu Zeilen |
| 192 | access X_OK EACCES | 0 | -13 | Execute-Bit nicht geprueft | access: X_OK vs i_mode |
| 193 | mono: woke at or after target | | | clock_nanosleep Timing | TSC Praezision |

## Lock-Granularitaet: Globale Locks → Linux-Vorbild

- [ ] Per-CPU Page Lists (page_alloc.c): buddy_lock → per-CPU Freelists
- [ ] Per-Inode Lock (ext2.c, vfs.c): fs_lock → rw_semaphore in vfs_node
- [ ] Per-Block Locking (bcache.c): Globales Lock → per-Block atomare Flags
- [ ] Per-CPU Slab (slab.c): Globale Freelist → per-CPU partial lists

## musl libc-test (20 FAIL)

- [ ] sem_open: MAP_SHARED Kohaerenz
- [ ] pthread_robust: Robust-Futex-Cleanup bei Thread-Exit
- [ ] malloc-brk-fail: brk VMA-Overlap-Check
- [ ] fma/fmal/powf/remquol: FPU-State Preservation
- [ ] tls_get_new-dtv: dlopen/TLS-Setup

## LTP (87 FAIL)

- [ ] execve: Execute-Bit pruefen (blockiert 60+ Tests)
- [ ] VMA/TLB Race: Atomarer VMA-Update + TLB-Shootdown
