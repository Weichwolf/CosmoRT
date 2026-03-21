# CosmoRT — Offene Punkte

Stand: 2026-03-21. Zweiter Audit gegen CosmoLib/CosmoPX.

Seit dem ersten Audit (2026-03-20) erledigt:
user_ok() Pointer-Validation, FS-Base per Thread, getrandom RDRAND,
Futex Blocking, VFS/ramfs, fork/exec/waitpid, Process Cleanup,
sched_yield, mmap Overflow-Check, ELF-Loader Failure-Cleanup,
E1000 + TCP/IP + Socket Syscalls.

---

## P0 — Sofort (Silent Corruption auf SMP)

### pages_alloc/pages_free ohne Lock
`page_alloc.c:81-103`. `pages_alloc()` und `pages_free()` greifen auf
die Bitmap ohne `page_lock` zu. `page_alloc()` hat den Lock. Auf SMP
koennen DMA-Allokation (`pages_alloc`) und Demand Paging (`page_alloc`)
gleichzeitig dieselbe Page vergeben.

Fix: `spin_lock_irq`/`spin_unlock_irq` um den Body beider Funktionen.

### sti vor Frame-Completion in Syscall-Entry
`syscall_entry.asm:23`. `sti` wird ausgefuehrt bevor der Frame-Pointer
in `[gs:32]` gespeichert ist (Zeile 43). Ein Timer-IRQ zwischen
Zeile 23 und 43 laesst `sched_preempt` einen unvollstaendigen Frame
lesen. Register-Korruption, falscher RIP nach Context Switch.

Fix: `sti` nach Zeile 43 verschieben (nach `mov [gs:32], rsp`).

### fork Race bei Page-Copy
`process.c:508-606`. `copy_address_space` kopiert Parent-Pages waehrend
der Parent auf einem anderen Core weiterlaeuft. Parent kann Pages
modifizieren die bereits kopiert wurden → stale Data im Child.
Schlimmer: mmap/munmap waehrend des Walks kann den VMA-Baum oder
Page-Tables korruptieren.

Fix: Copy-on-Write (Pages read-only markieren, bei Write-Fault kopieren).
Kurzfristig: alle Parent-Threads stoppen waehrend fork.

---

## P1 — Sicherheit (Exploitable)

### map_user_page ignoriert Protection-Flags
`process.c:121`. Hardcoded `PTE_PRESENT | PTE_WRITE | PTE_USER`.
NX-Bit wird nie gesetzt. Read-only VMAs sind schreibbar.
W^X Violation — Code Injection in .rodata moeglich.

Fix: Prot-Flags aus VMA durchreichen. `prot_to_pte_flags()` existiert
bereits in `syscall.c:337`, nur nicht benutzt in `map_user_page`.

### ASLR mit RDTSC
`process.c:164-167`. `aslr_rand()` nutzt RDTSC — monoton, vorhersagbar,
in QEMU-TCG deterministisch. RDRAND ist verfuegbar (wird in getrandom
bereits benutzt), wird hier aber nicht verwendet.

Fix: RDRAND statt RDTSC in `aslr_rand()`.

### user_ok(path, 1) prueft nur erstes Byte
`syscall.c:787`. String-Pointer-Validation prueft nur dass das erste
Byte im User-Space liegt. Ein String der bei `0x7FFFFFFFFFFF` beginnt
liest mit dem Null-Terminator-Scan in Kernel-Speicher.

Fix: `strnlen_user()` implementieren oder Path in Kernel-Buffer kopieren
(max 4096 Bytes) mit page-by-page Validation.

### do_clock_getres ohne user_ok
`syscall.c:683`. `tp->tv_sec = 0` schreibt ohne Pointer-Validation.
Kernel-Write an beliebige Adresse.

Fix: `if (tp && !user_ok((uint64_t)tp, 16)) return -EFAULT;`

### do_wait4 user_ok unvollstaendig
`process.c:721`. Prueft nur `< 0x800000000000`, kein Overflow-Check.
Ein wstatus-Pointer bei `0x7FFFFFFFFFFC` schreibt 4 Bytes ueber die
User/Kernel-Grenze.

Fix: `user_ok((uint64_t)wstatus, sizeof(int))` verwenden.

---

## P2 — Korrektheit (Funktional kaputt)

### Kein TLB Shootdown
`munmap`/`mprotect` flushen nur den lokalen TLB. Andere Cores
behalten stale Eintraege. Mit CLONE_VM-Threads: Core 2 greift
auf freigegebene Pages zu.

Fix: IPI an alle Cores mit gleichem PML4. Empfaenger: `invlpg`
oder CR3 Reload.

### fork FD-Sharing ohne Refcount
`process.c:531-535`. FD-Entries werden per Value kopiert. `vfs_file*`
wird geteilt ohne Referenzzaehlung. Close in einem Prozess gibt
die Struktur frei, der andere hat einen Dangling Pointer.

Fix: Refcount auf `vfs_file`. `fork` incrementiert, `close` decrementiert.

### do_clone Thread-Liste ohne Lock
`process.c:593-596`. `threads`-Liste und `thread_count` werden ohne
Lock modifiziert. Zwei concurrent `clone()` im selben Prozess
korruptieren die Liste.

Fix: `p->lock` um Thread-Listen-Modifikation.

### cwd ist global statt per-Process
`vfs.c:28`. `static char cwd[256]` wird von allen Prozessen geteilt.
`chdir` in einem Prozess aendert das Verzeichnis fuer alle.

Fix: cwd in `process_t` verschieben.

### execve ignoriert argv/envp
`process.c:611`. `(void)argv; (void)envp;` — Argumente werden nicht
an den neuen Prozess uebergeben. Shell kann keine Parameter an
Programme weitergeben. `./configure --prefix=/usr` funktioniert nicht.

Fix: argv/envp auf den neuen User-Stack kopieren (Linux ABI).

### Signal-Delivery an Userspace fehlt
`syscall.c:960`. `sigaction` registriert Handler, aber Delivery
(User-Stack modifizieren, RIP auf Handler setzen) ist nicht
implementiert. `sigreturn` fehlt ebenfalls.

Fix: Vor Return-to-Userspace pending Signals pruefen. Signal-Frame
auf User-Stack pushen. `sigreturn` Syscall zum Wiederherstellen.

### IPC blocking/waking nicht verbunden
`ipc.c:58,147`. `ipc_send` und `ipc_notify` haben TODOs fuer
Thread-Unblocking. `ipc_recv` returnt -2 ("would block") ohne
tatsaechlich zu blockieren.

Fix: An Futex-Muster anlehnen: THREAD_BLOCKED + Waitqueue.

---

## P3 — Fehlende Features (Bootstrap-Blocker)

Reihenfolge nach Abhaengigkeit:
Shell → configure → make → GCC → Ruby → Homebrew.

### pipe/pipe2
Shell-Pipelines (`./configure | grep`, `make 2>&1`). SYS_PIPE2
returnt -ENOSYS.

### mkdir/rmdir/unlink/rename
Dateisystem-Mutation. `make`, `tar`, `./configure` brauchen alle vier.

### getdents64
Verzeichnis-Enumeration. `ls`, `find`, `opendir`/`readdir`.

### ioctl/fcntl
Terminal-Control (TIOCGWINSZ), FD-Flags (F_GETFL/F_SETFL, F_DUPFD,
O_NONBLOCK). Beide returnen -ENOSYS.

### readv
CosmoPX stdio kann readv nutzen. SYS_READV returnt -ENOSYS.

---

## P4 — Robustheit

### net_poll static Buffer
`net.c:78-102`. `static uint8_t pkt[Q_PKT]` wird von allen Cores
in `sched_loop` beschrieben. SMP: Packet-Korruption.

Fix: Per-Core Buffer oder Lock/Trylock.

### sock_alloc ohne Lock
`socket.c:28-36`. Concurrent `socket()` kann denselben Slot vergeben.

Fix: Spinlock um Allokation.

### TCP Sequence/Port vorhersagbar
`net.c:246,285`. Ephemeral Port ab 49152 inkrementell, Seq=1000.
TCP Sequence Prediction Attack moeglich.

Fix: RDRAND fuer Initial Sequence Number und Ephemeral Port.

### do_write returnt falsche Laenge
`syscall.c:61`. Limitiert Output auf 64KB, returnt aber `count`.
Stille Daten-Trunkierung.

Fix: Tatsaechlich geschriebene Bytes returnen.

### Packet-Queues ohne Lock
`net.c:47-64`. `q_push`/`q_pop` ohne Synchronisation. SMP:
verlorene oder doppelt verarbeitete Pakete.

### Idle-Stacks zu klein
`sched.c:384`. 8KB Idle-Stacks. `vma_find_free` alloziert 16KB
auf dem Stack. Wenn Syscall-Kontext auf Idle-Stack laeuft: Overflow.
(Passiert nicht im Normalfall, aber fragil.)

Fix: Idle-Stacks auf 16KB oder 32KB erhoehen. IST fuer NMI/DF/MCE.

### Spinlock CAS statt Load-Wait
`spinlock.h:20`. CAS-Loop statt volatile Load erzeugt unnoetig
LOCK CMPXCHG Bus-Traffic bei Contention.

Fix: `__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE)` im Spin-Loop.

---

## Erledigt (seit 2026-03-20)

- [x] User-Pointer-Validation (`user_ok()` in syscall.c)
- [x] FS-Base per Thread (save/restore in sched_preempt)
- [x] getrandom CSPRNG (RDRAND + RDSEED)
- [x] Futex Blocking (THREAD_BLOCKED + Waitqueue)
- [x] VFS/Filesystem (ramfs, open/read/write/close/stat)
- [x] fork/exec/waitpid
- [x] Process Cleanup (proc_cleanup)
- [x] sched_yield
- [x] mmap/munmap Overflow-Check
- [x] ELF-Loader Failure-Cleanup (goto-pattern)
- [x] Networking (E1000 + TCP/IP + Socket Syscalls)
