# CosmoRT — Offene Punkte

Stand: ktest 2076/193, musl 452/20, LTP 11/87

## Strukturelle Schwaechen

### A. VFS Path-Resolution (vfs_lookup.c) — 15 Failures

`vfs_lookup` traversiert den Pfad und gibt bei jedem Fehler ENOENT. Linux's
`link_path_walk` prueft nach jeder Component: ist es ein Directory? Symlink-Tiefe?
Namenlaenge? Rewrite nach Linux-Vorbild `fs/namei.c:link_path_walk`.

- [ ] **ENOTDIR**: Pfad-Component die kein Directory ist (z.B. `/dev/null/foo`) → ENOTDIR statt ENOENT. Betrifft: access, chmod, chown, chdir, creat, utime, fchmodat, fchownat, execveat.
- [ ] **ELOOP**: Symlink-Rekursion >40 → ELOOP statt ENOENT. Betrifft: access, chmod, chown, fchownat.
- [ ] **ENAMETOOLONG**: Component >255 Bytes oder Gesamtpfad >4096 → ENAMETOOLONG statt ENOENT. Betrifft: chdir, creat, fchmodat.

### B. FD-Typ-System (fd.h, socket.c) — 10 Failures

Kein einheitlicher FD-Typ-Dispatch. Linux: `struct file` mit `f_op` Vtable. Socket-Ops,
Pipe-Ops, File-Ops sind verschiedene Implementierungen hinter einheitlichem FD.
CosmoRT: `fd_entry.type` existiert aber Syscalls dispatchen nicht konsistent darueber.

- [ ] **ENOTSOCK**: accept/bind/connect auf Non-Socket-FD gibt EBADF (-9) statt ENOTSOCK (-88). `sock_from_fd()` muss zweistufig pruefen: erst fd_lookup (EBADF wenn ungueltig), dann type-check (ENOTSOCK wenn kein Socket). Betrifft: accept (file, dir, pipe, epoll, eventfd, timerfd, inotify, memfd), bind (file, pipe), connect (file).
- [ ] **EOPNOTSUPP**: accept auf UDP-Socket gibt EINVAL (-22) statt EOPNOTSUPP (-95).

### C. Loopback-Netzwerk (net/, socket.c) — 18 Timeouts

Pakete an 127.0.0.1 muessen den NIC-Treiber umgehen und direkt in den Empfangspfad
eingespeist werden. CosmoRT hat keinen Loopback-Device. Zusaetzlich blockiert
`socket.c:669-671` Gateway-Check alle Sockets wenn `net_gw_ip==0`.

- [ ] **Loopback-Device**: Pakete an 127.0.0.0/8 direkt an `net_rx` zurueckleiten statt an NIC. Vorbild: Linux `drivers/net/loopback.c`.
- [ ] **Gateway-Check entfernen**: EAGAIN-Pruefung auf `net_gw_ip==0` blockiert Loopback-Connections.
- [ ] Betrifft: accept loopback, accept4, connect, alle TCP-Tests auf localhost.

### D. epoll Event-Delivery (epoll.c, pipe, socket) — 10 Failures

`epoll_wait` pollt aktiv via `fd_poll_readiness`. Kein Push-Notification von Datenquellen.
Linux: jeder `struct file` hat `poll`-Operation, `epoll_ctl(ADD)` registriert Callback,
Datenquelle ruft `wake_up` bei Events.

- [ ] **Pipe→epoll**: pipe_write muss epoll-Waiter aufwecken (EPOLLIN).
- [ ] **Socket→epoll**: accept/connect/recv muessen epoll-Waiter aufwecken.
- [ ] **EPOLLONESHOT**: epoll_wait nach erstem Event deaktiviert den FD nicht.
- [ ] **EPOLLET**: Edge-Triggered Semantik unvollstaendig.
- [ ] Betrifft: epoll_wait01-in, epoll_wait07, epoll_ctl01, epoll_pwait01/02/05, oneshot, ET.

### E. Nicht implementierte Syscalls (return 0 statt korrekte Implementierung) — 50 Failures

Syscalls die `return 0` statt korrekte Fehler/Ergebnisse liefern. Kein Stub-Patching
sondern pro Syscall gegen Linux-Source `kernel/` implementieren.

- [ ] **fadvise64** (SYS_FADVISE64=221): Komplett ignoriert. Muss EBADF bei ungueltigem fd, EINVAL bei ungueltigem advice, ESPIPE bei Pipe returnen. Vorbild: `mm/fadvise.c`. 19 Failures.
- [ ] **acct** (SYS_ACCT=163): Stub `return 0`. Muss EISDIR, ENOENT, ENOTDIR, EFAULT pruefen. Vorbild: `kernel/acct.c`. 4 Failures.
- [ ] **capget/capset** (SYS_CAPGET=125, SYS_CAPSET=126): Return -1 statt korrekte Capability-Structs. Muss Version-Header parsen, Capabilities lesen/schreiben. Vorbild: `kernel/capability.c`. 5 Failures.
- [ ] **adjtimex** (SYS_ADJTIMEX=159): EINVAL bei ungueltigem tick/mode nicht geprueft, EFAULT bei NULL-Pointer nicht geprueft. Vorbild: `kernel/time/ntp.c`. 4 Failures.
- [ ] **chroot** (SYS_CHROOT=161): Stub `return 0`. Muss Pfad-Validierung (ENOENT, ENOTDIR, ELOOP, ENAMETOOLONG) und tatsaechlichen Root-Wechsel implementieren. Vorbild: `fs/open.c:ksys_chroot`. 4 Failures.
- [ ] **clock_settime**: EINVAL bei ungueltigem clock_id/timespec nicht geprueft. Vorbild: `kernel/time/posixtime.c`. 4 Failures.
- [ ] **eventfd**: write(UINT64_MAX) muss EINVAL returnen (Overflow). Vorbild: `fs/eventfd.c`. 1 Failure.
- [ ] **fdatasync**: Auf Pipe muss EINVAL returnen. Vorbild: `fs/sync.c`. 1 Failure.
- [ ] **close_range**: Ungueltige Flags nicht geprueft. Vorbild: `fs/open.c`. 1 Failure.

### F. chmod/fchmod Mode-Bits — 8 Failures

Mode-Bits werden nicht korrekt gesetzt. `do_chmod`/`do_fchmod` aendert den VFS-Node
nicht oder ramfs ignoriert den Wert. Zusaetzlich: suid/sgid wird bei chown nicht geloescht.

- [ ] **chmod setzt Mode nicht**: Stat nach chmod zeigt alten Wert (0755/493). `vfs_node.mode` wird nicht aktualisiert.
- [ ] **suid/sgid bei chown**: Linux loescht SUID/SGID-Bits bei Ownership-Wechsel. CosmoRT nicht.
- [ ] **creat Mode**: creat ignoriert mode-Argument, setzt immer 0755.

### G. chown uid/gid Speicherung — 10 Failures

CosmoRT ist Single-User (immer uid=0), aber chown/fchown muss uid/gid im Inode
speichern. Programme (tar, cp -a) setzen Ownership und erwarten es zuruecklesen
zu koennen.

- [ ] **uid/gid in vfs_node**: Felder hinzufuegen, in do_chown/do_fchown setzen, in do_stat zurueckgeben.
- [ ] Betrifft: chown01-05, fchown01-05, fchownat01-03, link uid/gid.

### H. clone/clone3 Flag-Validierung — 6 Failures

Ungueltige Flag-Kombinationen werden nicht abgelehnt. Linux hat >20 Regeln.

- [ ] **NULL-Stack bei CLONE_VM**: Muss EINVAL returnen.
- [ ] **CLONE_SIGHAND ohne CLONE_VM**: Muss EINVAL returnen.
- [ ] **CLONE_THREAD ohne CLONE_SIGHAND**: Muss EINVAL returnen.
- [ ] **clone3 size-Validierung**: Zu kleine `clone_args` Struktur → EINVAL.
- [ ] **clone3 stack ohne stack_size**: Muss EINVAL returnen.
- [ ] Vorbild: `kernel/fork.c:copy_process` Flag-Checks.

### I. Signal-Delivery bei blockierten Syscalls — 3 Failures

Teilweise gefixt (event_wait prueft jetzt alle Signals). Aber nanosleep-Restart
gibt -EINTR bevor der Handler gelaufen ist.

- [ ] **nanosleep SIGALRM**: Handler muss VOR -EINTR laufen. Aktuell: Syscall restartet, do_nanosleep sieht `now < deadline`, gibt -EINTR, DANN laeuft check_pending_signals. Reihenfolge ist korrekt (Linux macht es genauso), aber der alarm-Test erwartet dass `alarms_received` schon 1 ist wenn nanosleep returned.
- [ ] **clock_nanosleep**: Gleicher Bug.
- [ ] **fcntl F_SETLKW**: Lock-Warten wird durch Signal nicht unterbrochen.

### J. Sonstige Einzelfehler

- [ ] **epoll_create Validierung**: Ungueltige Argumente (0, -1, bad flags) muessen EINVAL returnen. Aktuell: erstellt trotzdem eine Instanz.
- [ ] **dup3 Flags**: Ungueltige flags muessen EINVAL returnen.
- [ ] **flock EBADF**: flock(-1) muss EBADF returnen.
- [ ] **flock EINVAL**: SH|EX zusammen, LOCK_NB allein muessen EINVAL returnen.
- [ ] **fcntl F_SETLK Pipe**: F_SETLK auf Pipe muss EINVAL returnen.
- [ ] **fcntl F_SETLK bad type**: Ungueltiger lock-Typ muss EINVAL returnen.
- [ ] **execveat**: Return -ENOSYS statt korrekte Fehler (EBADF, EINVAL, ENOTDIR, ELOOP).
- [ ] **fchmodat2 EINVAL**: Ungueltige Flags nicht geprueft.
- [ ] **fchownat EINVAL**: Ungueltige Flags nicht geprueft.
- [ ] **64-bit Timestamps**: atime/mtime 32-bit Truncation. `vfs_node.atime/mtime/ctime` auf int64_t erweitern.
- [ ] **Timestamps in vfs_ioctls.c**: 32-Bit-Truncation in Zeile 369/393.
- [ ] **epoll_wait auf Non-Epoll-FD**: Muss EINVAL returnen statt EBADF.
- [ ] **fd==epfd bei epoll_ctl**: Muss EINVAL returnen.

## Lock-Granularitaet: Globale Locks → Linux-Vorbild

Globale Locks blockieren alle Cores — inkompatibel mit RT. Linux-Vorbild: per-CPU, per-Object, per-Zone.

- [ ] **Per-CPU Page Lists** (page_alloc.c): `buddy_lock` global → per-CPU Freelists mit Batch-Refill. Vorbild: Linux `struct per_cpu_pages`, Batch 31.
- [ ] **Per-Inode Lock** (ext2.c, vfs.c): `fs_lock` global → rw_semaphore in `vfs_node`. Vorbild: Linux `inode->i_rwsem`.
- [ ] **Per-Block Locking** (bcache.c): Globales bcache-Lock → per-Block atomare Flags. Vorbild: Linux `bh->b_state` Bitops.
- [ ] **Per-CPU Slab** (slab.c): Globale Freelist → per-CPU partial lists. Vorbild: Linux SLUB per-CPU Caches.

## musl libc-test Fixes (20 FAIL → 0)

- [ ] sem_open: MAP_SHARED Kohaerenz — ramfs liefert unterschiedliche Pages statt shared.
- [ ] pthread_robust: Robust-Futex-Cleanup bei Thread/Process-Exit fehlt.
- [ ] malloc-brk-fail: brk VMA-Overlap-Check fehlt.
- [ ] fma/fmal/powf/remquol: FPU-State wird bei Context-Switch/Syscall clobbered.
- [ ] tls_get_new-dtv: dlopen SEGFAULT — dynamischer Linker kann .so nicht laden.

## Linux-Konformitaet (LTP / Allgemein)

- [ ] execve: Execute-Bit pruefen (`process_exec.c:241`). Blockiert 60+ LTP-Tests.
- [ ] VMA/TLB Race: VMA-Update und TLB-Shootdown atomar machen. SMP-Crashes bei chdir04/chmod06.
