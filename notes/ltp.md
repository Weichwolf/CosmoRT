# LTP/ktest Fail-Analyse — 2026-04-02

Stand: **2255/98** ktest (vorher 2238/109)
LTP haengt bei chdir04 (Test 43/313) → 270 Tests nie gelaufen.

## Erledigte Fixes

- [x] #0 VFS ENAMETOOLONG: ext2_walk_err mit korrekten Fehlercodes
- [x] #1 VFS Error-Codes: resolve_path Slash-Collapse, execve vfs_stat Vorab-Check,
      O_CREAT nur bei ENOENT, devfs ENOTDIR, vfs_path_error Fallback
- [x] VFS Refactoring: Mount-Table + ops-Dispatch (ext2_vfs.c, tmpfs.c, devfs.c)
- [x] Epoll: fd==epfd EINVAL, nesting>5 EINVAL, circular ELOOP

## Uebersicht

| #  | Subsystem        | Fails | Komplexitaet | Dateien                                           |
|----|------------------|-------|--------------|---------------------------------------------------|
| 0  | VFS ENAMETOOLONG | 1+~10 | NIEDRIG      | vfs_lookup.c                                      |
| 1  | VFS Error-Codes  | 10    | MITTEL       | vfs_lookup.c, vfs.c, vfs_symlink.c                |
| 2  | Clone/Threads    | 6     | MITTEL       | process_fork.c, sys_proc.c, futex.c               |
| 3  | Loopback TCP     | 14    | HOCH         | tcp.c, ip.c, socket.c                             |
| 4  | Signale          | 3     | MITTEL       | signal.c, signal_handler.c, process_wait.c        |
| 5  | Fork/Exec        | 6     | MITTEL       | process_fork.c, process_exec.c, sys_proc.c        |
| 6  | File Permissions | 8     | NIEDRIG      | vfs.c, vfs_ioctls.c, vfs_rw.c, sys_file.c        |
| 7  | Epoll/Eventfd    | 11    | MITTEL       | epoll.c, eventfd.c, stubs.c                       |
| 8  | Fcntl/Flock      | 12    | MITTEL       | sys_file.c, stubs.c                               |
| 9  | Capabilities     | 9     | NIEDRIG      | stubs.c                                           |
| 10 | Syscall-Stubs    | 12    | NIEDRIG      | stubs.c, sys_fs.c, signal_handler.c               |
| 11 | Timestamps       | 2     | —            | ext2.c (nicht fixbar ohne ext2-Erweiterung)       |
| 12 | Sonstiges        | 5     | MIXED        | procfs.c, process.c, sched.c                      |

---

## #0 — VFS ENAMETOOLONG (BLOCKER)

**Fails:** chdir04 HUNG (blockiert 270 LTP-Tests), creat ENAMETOOLONG, ENAMETOOLONG allgemein
**Impact:** Blockiert die gesamte LTP-Suite ab Test 43.
**Root Cause:** chdir04 ruft `chdir(300-Byte-String)` auf. vfs_lookup_impl() hat den
NAME_MAX-Check (vfs_lookup.c:42-45), aber der Pfad wird vorher als einzelne Komponente
ohne `/` geparst. Wenn der Name kein `/` enthaelt, wird `len = strlen(path)` und der
Component-Loop laeuft einmal mit len=299. Der Check `len > NAME_MAX` (255) sollte greifen
und -ENAMETOOLONG setzen.

**Vermutung:** Der Check greift, aber der Aufrufer (sys_chdir) propagiert den Fehlercode
nicht. Oder: vfs_lookup_impl iteriert endlos bei bestimmten Pfadmustern.

**Dateien:**
- `src/kernel/fs/vfs_lookup.c:42-45` — NAME_MAX Check
- `src/kernel/sys/sys_fs.c` — sys_chdir Aufrufer

**Fix:**
1. Verifizieren dass vfs_lookup_err() den *err-Pointer korrekt setzt bei ENAMETOOLONG
2. Sicherstellen dass sys_chdir den Fehlercode von vfs_lookup_err propagiert
3. Testen: chdir mit 300-Byte-String muss -ENAMETOOLONG liefern, nicht haengen

---

## #1 — VFS Error-Codes

**Fails:**
- `slash paths all resolve to /` — chdir("///") liefert falsches getcwd (4 Fehler)
- `chroot ELOOP` — got -ENOENT, expected -ELOOP
- `circular ELOOP` — Symlink-Loop nicht erkannt (got 0)
- `nesting > 5 fails` — Symlink-Tiefe nicht begrenzt
- `ENOTDIR` (2x) — got -ENOENT statt -ENOTDIR
- `ENAMETOOLONG` — got -ENOENT statt -ENAMETOOLONG
- `utime ENOTDIR` — got -ENOENT statt -ENOTDIR
- `mmap file` — file-backed mmap

**Root Cause:** Viele VFS-Operationen rufen vfs_lookup() auf und pruefen nur `== NULL`,
ohne den konkreten Fehlercode aus `*err` weiterzugeben. Ergebnis: alles wird -ENOENT.

**Dateien:**
- `src/kernel/fs/vfs_lookup.c:26` — ELOOP bei depth > SYMLOOP_MAX (40), funktioniert
- `src/kernel/fs/vfs_lookup.c:42-45` — ENAMETOOLONG Check, funktioniert
- `src/kernel/fs/vfs_lookup.c:51` — ENOTDIR Check, funktioniert
- `src/kernel/fs/vfs.c` — vfs_open() und andere Wrapper propagieren *err nicht

**Fix:**
1. Alle vfs_open/vfs_stat/vfs_*-Aufrufer auf vfs_lookup_err() umstellen
2. Fehlercode aus *err zurueckgeben statt pauschal -ENOENT
3. Slash-only-Pfade: chdir("///") muss nach "/" normalisieren, getcwd muss "/" liefern
4. File-backed mmap: vfs_mmap muss page_cache-Mapping unterstuetzen

**Abhaengigkeit:** #0 muss zuerst gefixt werden.

---

## #2 — Clone/Threads

**Fails:**
- `clone returns tid` (2x) — falscher Rueckgabewert
- `worker thread ran` — Thread wurde nicht gescheduled
- `child_tid cleared to 0` — got -1, expected 0
- `created >= 64 threads` — got 0, kein einziger Thread erstellt
- `WAIT timeout = -ETIMEDOUT` — futex_wait liefert 0 statt -110

**Dateien:**
- `src/kernel/proc/process_fork.c:186-349` — kernel_clone()
- `src/kernel/proc/process_fork.c:310-317` — CLONE_CHILD_CLEARTID Setup
- `src/kernel/sys/sys_proc.c:179-188` — child_tid clear + futex_wake auf Exit
- `src/kernel/sys/sys_proc.c:246-295` — do_clone/do_clone3 Syscall-Handler
- `src/kernel/ipc/futex.c:154-265` — futex_wait/futex_wake

**Analyse:**
- CLONE_CHILD_CLEARTID ist implementiert (process_fork.c:315-317, sys_proc.c:179-188)
- child_tid wird auf Exit auf 0 gesetzt + futex_wake aufgerufen
- Problem: child_tid got=-1 statt 0 → copy_to_user schlaegt fehl oder Adresse ungueltig
- 0 Threads erstellt → clone() Syscall liefert Fehler oder Flags-Validierung zu streng
- futex_wait return 0 statt -ETIMEDOUT → Event-Queue verschluckt Timeout-Event

**Fix:**
1. clone() Rueckgabewert pruefen: Parent bekommt TID, Child bekommt 0
2. Flag-Validierung in kernel_clone:190-192 gegen Linux abgleichen
3. child_tid clear: Adress-Mapping pruefen (CR3 Switch vor copy_to_user)
4. futex_wait Timeout: event_wait muss -ETIMEDOUT propagieren wenn Timer feuert
5. Thread-Erstellung: slab_alloc fuer thread_t pruefen (64 Threads = Slab-Exhaustion?)

---

## #3 — Loopback TCP

**Fails:** 14 Tests — accept4 (5x), accept loopback (2x), connect (3x), nonblock (2x),
data matches, connect ECONNREFUSED

**Root Cause:** TCP Hash-Lookup-Mismatch bei Loopback.

Wenn connect(127.0.0.1:8080) aufgerufen wird:
- Registriert: `local_port=EPHEMERAL, remote_port=8080, dst_ip=127.0.0.1`
- SYN-Paket wird ueber loopback_inject() in q_tcp injiziert

Wenn tcp_input() das SYN empfaengt:
- `tcp_find(dport=8080, sport=EPHEMERAL, src_ip=127.0.0.1)` sucht:
  - local_port=8080 (Server-Port)
  - remote_port=EPHEMERAL
- Findet KEINEN Match, weil der registrierte Eintrag die Ports andersrum hat

**Dateien:**
- `src/kernel/net/ip.c:18-44` — loopback_inject()
- `src/kernel/net/tcp.c:93-137` — tcp_find() Hash-Lookup
- `src/kernel/net/tcp.c:948-979` — net_tcp_connect() + tcp_register()
- `src/kernel/net/tcp.c:674-686` — tcp_input() Lookup
- `src/kernel/net/tcp.c:881-944` — net_tcp_accept()
- `src/kernel/net/socket.c:609-695` — accept() Syscall-Path

**Fix:**
1. tcp_input() muss bei fehlgeschlagenem tcp_find() den Listener-Socket checken
   (passiv-Verbindung: SYN an gebundenen Port ohne bestehende Connection)
2. Listener-Lookup: Nach local_port=dport im Listener-Set suchen
3. Neues tcp_conn_t fuer die akzeptierte Verbindung anlegen (SYN-RECEIVED State)
4. connect ECONNREFUSED: Wenn kein Listener → RST senden, connect mit -ECONNREFUSED wecken
5. SOCK_NONBLOCK/CLOEXEC Flags bei accept4 auf neuen FD uebertragen

---

## #4 — Signale

**Fails:**
- `SIGCONT: WIFCONTINUED` — waitpid meldet SIGCONT nicht
- `SIGCHLD received` — got 0, Signal nicht zugestellt
- `alarm(1) old=0` — alarm() gibt 5 statt 0 zurueck

**Dateien:**
- `src/kernel/sys/sys_proc.c:115-128` — SIGCHLD auf Child-Exit
- `src/kernel/proc/signal.c:93-121` — SIGCONT Handler, setzt was_continued
- `src/kernel/proc/process_wait.c:131-140` — WCONTINUED Check
- `src/kernel/proc/signal_handler.c:108-127` — do_alarm()

**Analyse:**
- SIGCHLD: Wird gesendet (sys_proc.c:115-128), sig_pending gesetzt, EQ_CHILD_EXITED
  gepostet. Problem: Signal-Delivery an Userspace (sigaction-Dispatch) koennte
  den Handler nicht aufrufen wenn SA_SIGINFO nicht korrekt verarbeitet wird.
- WIFCONTINUED: Logik in process_wait.c:131-140 sieht korrekt aus (0xFFFF Status).
  Problem: was_continued wird in signal.c:108 gesetzt, aber der Event
  EQ_CHILD_CONTINUED muss den wartenden Parent wecken.
- alarm(1) gibt 5 zurueck: alarm_deadline_ms ist beim ersten Aufruf != 0.
  Slab-Reuse: proc_alloc() macht kmemset(p,0,sizeof), aber alarm_deadline_ms
  koennte nach kmemset durch init-Code ueberschrieben werden.

**Fix:**
1. SIGCHLD: Signal-Delivery pruefen — wird der Userspace-Handler aufgerufen?
   SA_NOCLDSTOP/SA_NOCLDWAIT Flags beachten.
2. WIFCONTINUED: event_post(EQ_CHILD_CONTINUED) muss Parent-wait4 wecken
3. alarm: proc_alloc() → alarm_deadline_ms Initialisierung tracen.
   Vermutung: ktest-init-Prozess hat alarm gesetzt, Child erbt via fork.

---

## #5 — Fork/Exec

**Fails:**
- `child exit code` — got 2, expected 0
- `child inherited memory` — fork CoW kaputt
- `fd still open` — FD-Table nicht korrekt kopiert
- `time advanced >= 9s` — clock_gettime nach fork liefert 0
- `execveat` (4x) — komplett -ENOSYS

**Dateien:**
- `src/kernel/proc/process_fork.c:66-136` — copy_one_vma() CoW
- `src/kernel/proc/process_fork.c:168-184` — dup_fd_table()
- `src/kernel/proc/process_exec.c` — do_execve
- `src/kernel/sys/syscall_table.h:408` — execveat → -ENOSYS
- `src/kernel/sys/sys_proc.c:198` — Exit-Code in Zombie

**Analyse:**
- CoW: Implementierung sieht korrekt aus (PTE_COW, Refcounting). Problem evtl.
  Page-Fault-Handler der neue Seite nicht korrekt mappt.
- FD-Table: dup_fd_table kopiert entries inkl. O_CLOEXEC (process_fork.c:170).
  Problem: Refcounting auf file-Strukturen?
- Exit-Code: Zombie speichert exit_code, wait4 liest ihn. Encoding pruefen
  (Linux: `(code & 0xff) << 8`).
- execveat: Nicht implementiert. Braucht AT_FDCWD + dirfd Pfadaufloesung.

**Fix:**
1. execveat implementieren: do_execve erweitern um dirfd-Parameter + AT_EMPTY_PATH
2. Exit-Code: wait-Status Encoding pruefen (`(exit_code & 0xff) << 8 | signal`)
3. CoW: Page-Fault-Handler testen — nach fork in Child schreiben, Parent unveraendert?
4. FD-Table: file-Refcount bei dup_fd_table incrementieren pruefen
5. clock_gettime nach fork: CLOCK_MONOTONIC muss weiterlaufen

---

## #6 — File Permissions

**Fails:**
- `mode bits` (3x) — creat/chmod ignoriert mode, liefert Default (448=0700, 384=0600)
- `mode 0000` — creat mode 0000 ergibt 0644 (420 dezimal)
- `suid/sgid cleared` — Write loescht suid/sgid nicht (3072 = S_ISUID|S_ISGID)
- `sgid preserved` — sgid geht verloren bei chmod
- `access X_OK EACCES` — Execute-Permission nicht geprueft
- `FD_CLOEXEC set` — O_CLOEXEC wird nicht als FD_CLOEXEC sichtbar

**Dateien:**
- `src/kernel/fs/vfs.c:538-539` — creat mode: `mode ? (mode & 07777) : 0644`
- `src/kernel/fs/vfs_ioctls.c:241-259` — chmod ext2/ramfs
- `src/kernel/fs/vfs_rw.c:167-242` — write() (kein suid/sgid clearing)
- `src/kernel/sys/sys_file.c:1002-1007` — fcntl F_GETFD/F_SETFD

**Fix:**
1. creat mode 0000: `mode ? ... : 0644` ist falsch — mode=0 ist ein gueltiger Wert.
   Fix: immer `mode & 07777` anwenden, umask separat (oder ignorieren, single-user).
2. suid/sgid auf write: In vfs_write() nach erfolgreichem Schreiben S_ISUID|S_ISGID
   aus inode->mode loeschen (Linux: `inode_kiocb_set_flags` / `file_remove_privs`).
3. sgid bei chmod: fchmod muss S_ISGID erhalten wenn explizit gesetzt.
4. access X_OK: inode->mode Exec-Bits pruefen.
5. FD_CLOEXEC: fcntl(F_GETFD) muss `(flags & O_CLOEXEC) ? FD_CLOEXEC : 0` liefern.
   Pruefen ob FD_CLOEXEC == 1 und O_CLOEXEC korrekt gemappt.

---

## #7 — Epoll/Eventfd

**Fails:**
- `epoll_pwait2 tv_sec<0 EINVAL` — keine Timeout-Validierung
- `epoll_pwait2 tv_nsec<0 EINVAL` — keine Timeout-Validierung
- `epoll_pwait2 tv_nsec>=1e9 EINVAL` — keine Timeout-Validierung
- `fd==epfd EINVAL` — epoll auf sich selbst erlaubt
- `epoll_wait on pipe EINVAL` — EBADF statt EINVAL
- `oneshot second write` — EPOLLONESHOT Semantik
- `sem value` (3x) — EFD_SEMAPHORE nicht implementiert
- `sem read` (2x) — EFD_SEMAPHORE read liefert EAGAIN statt 8
- `write UINT64_MAX EINVAL` — eventfd Overflow-Check fehlt

**Dateien:**
- `src/kernel/sys/stubs.c:275-284` — epoll_pwait2, KEINE Timeout-Validierung
- `src/kernel/event/epoll.c:82-172` — do_epoll_ctl, KEIN Self-Add-Check
- `src/kernel/event/eventfd.c:25-48` — do_eventfd2, KEIN EFD_SEMAPHORE

**Fix:**
1. epoll_pwait2: Vor Timeout-Berechnung pruefen: `tv_sec < 0 || tv_nsec < 0 ||
   tv_nsec >= 1000000000` → -EINVAL
2. epoll_ctl: `if (epfd == fd) return -EINVAL;` in EPOLL_CTL_ADD
3. epoll_wait auf non-epoll fd: Typ-Check → -EINVAL statt -EBADF
4. EPOLLONESHOT: Nach erstem Event die Registrierung deaktivieren (nicht loeschen)
5. EFD_SEMAPHORE: read liefert 1 und dekrementiert Counter um 1 (statt alles lesen)
6. eventfd write UINT64_MAX: `if (value == UINT64_MAX) return -EINVAL;`
   Overflow-Check: `if (counter + value < counter) block oder -EAGAIN`

---

## #8 — Fcntl/Flock

**Fails:**
- `F_SETLK pipe EINVAL` (2x) — Lock auf Pipe nicht abgelehnt
- `F_SETLK bad type EINVAL` — Ungueltiger Lock-Typ nicht erkannt
- `partial unlock` — Lock-Range-Splitting fehlt
- `child got lock after wait` — F_SETLKW blockiert nicht
- `flock(-1) EBADF` — fd-Validierung fehlt
- `LOCK_NB alone EINVAL` — LOCK_NB ohne SH/EX
- `SH|EX EINVAL` — LOCK_SH|LOCK_EX gleichzeitig
- `fd2 EX denied` — Exclusive Lock blockiert nicht
- `child SH ok, EX blocked` — SH/EX Interaktion
- `child SH+EX both blocked` — Mehrfach-Lock
- `child got EWOULDBLOCK` — LOCK_NB sollte -EWOULDBLOCK liefern

**Dateien:**
- `src/kernel/sys/stubs.c:29-32` — flock() ist No-Op (return 0)
- `src/kernel/sys/sys_file.c:1026-1033` — F_SETLK Handler
- `src/kernel/sys/sys_file.c:936-969` — flock_setlk()

**Analyse:**
- flock() ist komplett Stub (return 0). Alle flock-Tests failen daher.
- F_SETLK: Pipes werden nicht abgelehnt (return 0 statt -EINVAL).
  Linux: Pipes/Sockets sind nicht lock-faehig.
- F_SETLKW: Blockiert nicht, weil keine Warteliste implementiert.
- Range-Splitting bei partiellem Unlock fehlt komplett.

**Fix:**
1. flock(): Implementieren mit per-inode Lock-Liste.
   - LOCK_SH: Shared (mehrere Reader), LOCK_EX: Exclusive (ein Writer)
   - LOCK_NB: Non-blocking, -EWOULDBLOCK bei Konflikt
   - Validierung: LOCK_NB allein → -EINVAL, SH|EX → -EINVAL
   - fd=-1 → -EBADF
2. F_SETLK: Pipes/Sockets → -EINVAL. Lock-Typ Validierung (F_RDLCK/F_WRLCK/F_UNLCK).
3. F_SETLKW: Warteliste + Event-basiertes Blocking.
4. Range-Splitting: Bei partiellem Unlock bestehenden Lock aufteilen.

---

## #9 — Capabilities

**Fails:** capget (4x), capset (4x), kernel sets v3 (1x) — alle 9 Tests

**Dateien:**
- `src/kernel/sys/stubs.c:25-26` — capget/capset returnen -EPERM

**Analyse:**
Single-user Kernel, kein Enforcement. Aber LTP erwartet funktionierendes capget/capset.
Linux-Verhalten: capget liefert aktuelle Capabilities, capset setzt sie (root darf alles).

**Fix:**
capget/capset mit Mindest-Logik implementieren:
1. Version-Handshake: capget mit NULL data + header.pid=0 liefert aktuelle Version
   (0x20080522 = v3) in header.version zurueck.
2. capget mit data: Alle Bits auf 1 (full caps, wir sind root).
3. capset: Validieren und akzeptieren (ignorieren, single-user).
4. pid-Validierung: pid=0 → current, pid=-1 → -EINVAL, pid=nonexistent → -ESRCH.

Aufwand: ~50 Zeilen. Kein Enforcement noetig.

---

## #10 — Syscall-Stubs

**Fails:**
- `acct` (5x) — return 0 statt Fehlercodes (EFAULT, ENOTDIR, ENOENT, EISDIR)
- `invalid mode EINVAL` — chmod Mode-Validierung
- `double bind EINVAL` — bind() doppelt auf gleichen Socket
- `alarm returns` (3x) — alter Wert falsch
- `adjtimex SET_MODE` — adjtimex nicht implementiert
- `fdatasync(pipe) EINVAL` — fdatasync auf Pipe akzeptiert
- `fallocate KEEP_SIZE` — FALLOC_FL_KEEP_SIZE nicht implementiert

**Dateien:**
- `src/kernel/sys/stubs.c:157-158` — acct: return 0
- `src/kernel/sys/stubs.c:123-135` — adjtimex: Partial
- `src/kernel/sys/stubs.c:93` — fdatasync → fsync (Pipe nicht abgelehnt)
- `src/kernel/sys/sys_fs.c:199-211` — fallocate (nur mode=0)
- `src/kernel/proc/signal_handler.c:108-127` — alarm

**Fix:**
1. acct(): Pfad validieren — NULL→-EFAULT, ENOTDIR/ENOENT/EISDIR pruefen.
   Danach -ENOSYS (wir implementieren Accounting nicht, aber Fehlercodes muessen stimmen).
2. adjtimex: ADJ_SETOFFSET implementieren oder zumindest akzeptieren.
3. fdatasync: Pipe/Socket → -EINVAL.
4. fallocate: FALLOC_FL_KEEP_SIZE Flag unterstuetzen (Platz allozieren ohne Datei
   zu vergroessern). Auf ext2: Bloecke pre-allozieren oder als No-Op akzeptieren.
5. alarm: Initialisierung pruefen. Wenn alarm_deadline_ms nach proc_alloc != 0,
   ist Slab-Reuse das Problem. Explizit `p->alarm_deadline_ms = 0` nach kmemset.
6. chmod: Mode 07777 Maske + EINVAL fuer ungueltige Bits.
7. bind doppelt: Wenn Socket bereits gebunden → -EINVAL.

---

## #11 — Timestamps (WONTFIX)

**Fails:**
- `atime 64-bit` — got 0, expected 4294967296 (2^32)
- `mtime 64-bit` — got 0, expected 4294967296 (2^32)

**Root Cause:** ext2 speichert Timestamps als uint32_t (Sekunden seit Epoch).
Werte >= 2^32 (nach 2106) ueberlaufen. Das ist eine Limitation des ext2-Formats.

**Fix:** Keiner ohne Dateisystem-Wechsel (ext4 hat 64-bit Timestamps).
Akzeptierter Fail.

---

## #12 — Sonstiges

**Fails:**
- `ktest on compute core` — Test laeuft nicht auf AP (SMP)
- `read /proc/stat` — procfs Stub liefert unerwartetes Format
- `regular file EPERM` — ioctl auf regular file
- `child exited normally (stack overflow)` — Guard-Page fehlt
- `flistxattr zero-size` — xattr nicht implementiert

**Dateien:**
- `src/kernel/core/sched.c` — SMP Scheduling, ktest-Dispatch auf AP
- `src/kernel/fs/procfs.c:433-463` — /proc/stat: alle Zeiten 0 (Fake)
- `src/kernel/proc/process.c:311-317` — User-Stack: VMA_GROWSDOWN, keine Guard-Page
- xattr: Nicht implementiert

**Fix:**
1. SMP ktest: sched_enqueue muss Tasks auf APs verteilen. Pruefen ob AP
   idle-loop korrekt schedule() aufruft.
2. /proc/stat: CPU-Zeiten aus sched-Accounting fuellen (runtime_ns pro Thread).
   Minimum: user/system/idle aufschluesseln.
3. Guard-Page: Unterhalb des Stack-VMA eine nicht-gemappte Page einfuegen.
   Page-Fault auf Guard-Page → SIGSEGV statt GROWSDOWN-Expansion.
   Stack-Bottom = stack_start - USER_STACK_INIT, Guard = stack_start - USER_STACK_INIT - PAGE_SIZE.
4. flistxattr: Stub mit -ENOTSUP oder -ERANGE je nach Buffergroesse.
   xattr auf tmpfs/ext2 ist optional.
5. ioctl auf regular file: -ENOTTY statt -EPERM (Linux-Verhalten).
