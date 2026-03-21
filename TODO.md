# CosmoRT — Offene Punkte

Stand: 2026-03-21.

ktest Ergebnis: 27 PASS, 0 FAIL, 3 SKIP (getrandom, VFS read, sched_yield).

---

## P0 — Sofort (Silent Corruption / Crash)

### sched_yield Kernel NULL-Deref
`syscall.c`: `save_user_state_for_block` liest `percpu->syscall_frame`
das NULL ist. ktest: KERNEL PANIC bei sched_yield.

Root Cause: `syscall_frame` wird in syscall_entry.asm gesetzt, aber
`save_user_state_for_block` wird aus dem C-Syscall-Handler aufgerufen
wo der Frame-Pointer gueltig sein sollte. Moeglicherweise wird der
Compiler-generierte Code zwischen Frame-Save und Handler-Aufruf
den Stack so umorganisiert dass `percpu->syscall_frame` ueberschrieben
wird. Oder: INT 0x80 Pfad setzt `syscall_frame` nicht.

Fix: Debug mit RIP-Output im Exception-Handler. Pruefen ob INT 0x80
oder SYSCALL Pfad. Frame-Pointer vor save_user_state_for_block pruefen.

### VFS read #GP
ktest: `open("/test.txt", O_RDONLY)` → PASS, dann `read(fd, buf, 32)` → #GP.
VFS write funktioniert, VFS read crasht.

Moegliche Ursache: `vfs_read` dereferenziert einen Pointer der in den
Kernel-Adressraum zeigt (EFI-relocated Symbol als Daten-Pointer).
Oder: FD_FILE Dispatch in do_read hat einen Bug.

Fix: Serial-Debug in vfs_read. RIP im Exception-Handler ausgeben.

### getrandom CPUID #GP
`check_rdrand()` mit CPUID.01H verursacht #GP trotz PIC-safe
xchg-Trick. Auch `-cpu max` hilft nicht.

Workaround: getrandom hardcoded -EIO. Funktioniert fuer Boot.
Langfristiger Fix: CPUID in memops_init (NASM, kein PIC-Problem)
ausfuehren, Ergebnis in globaler Variable speichern.

### fork Race bei Page-Copy
`copy_address_space` kopiert Parent-Pages waehrend Parent weiterlaeuft.
SMP: stale Data im Child, VMA-Baum-Korruption moeglich.

Fix: Copy-on-Write oder Parent-Threads stoppen waehrend fork.

### ~~pages_alloc/pages_free ohne Lock~~
Erledigt: pages_alloc/pages_free haben seit Refactor Spinlocks.

### ~~sti vor Frame-Completion~~
Erledigt: sti nach Frame-Save verschoben (61762fa).

---

## P1 — Sicherheit (Exploitable)

### map_user_page ignoriert Protection-Flags
Hardcoded `PTE_PRESENT | PTE_WRITE | PTE_USER`. NX-Bit nie gesetzt.
Read-only VMAs sind schreibbar. W^X Violation.

### ASLR mit RDTSC
`aslr_rand()` nutzt RDTSC — vorhersagbar. Sollte RDRAND nutzen
(wenn CPUID-Bug gefixt).

### user_ok(path, 1) prueft nur erstes Byte
String-Pointer-Validation prueft nicht die gesamte String-Laenge.
Kernel-Memory-Read moeglich.

### do_clock_getres ohne user_ok
Schreibt ohne Pointer-Validation.

### do_wait4 user_ok unvollstaendig
Kein Overflow-Check auf wstatus-Pointer.

---

## P2 — Korrektheit (Funktional kaputt)

### Kein TLB Shootdown
munmap/mprotect flushen nur lokalen TLB. SMP mit CLONE_VM-Threads:
stale Pages auf anderen Cores.

### fork FD-Sharing ohne Refcount
vfs_file* wird geteilt ohne Referenzzaehlung. Close → Dangling Pointer.

### do_clone Thread-Liste ohne Lock
Concurrent clone() im selben Prozess korruptiert Thread-Liste.

### cwd ist global statt per-Process
Alle Prozesse teilen ein cwd.

### execve ignoriert argv/envp
Shell kann keine Parameter an Programme weitergeben.

### Signal-Delivery an Userspace fehlt
sigaction registriert Handler, Delivery nicht implementiert.

### IPC blocking/waking nicht verbunden
ipc_recv returnt -2 statt zu blockieren.

---

## P3 — Fehlende Features (Bootstrap-Blocker)

Shell → configure → make → GCC → Ruby → Homebrew.

- pipe/pipe2 (Shell-Pipelines)
- mkdir/rmdir/unlink/rename (Dateisystem-Mutation)
- getdents64 (ls, find, readdir)
- ioctl/fcntl (Terminal-Control, FD-Flags)
- readv (CosmoPX stdio)

---

## P4 — Robustheit

- net_poll static Buffer (SMP Packet-Korruption)
- sock_alloc ohne Lock (concurrent socket())
- TCP Sequence/Port vorhersagbar (RDRAND statt inkrementell)
- do_write returnt falsche Laenge (64KB Limit, returnt count)
- Packet-Queues ohne Lock (SMP)
- Idle-Stacks 8KB zu klein (vma_find_free braucht 16KB+)
- Spinlock CAS statt Load-Wait (unnoetig LOCK CMPXCHG)

---

## Erledigt

- [x] User-Pointer-Validation (user_ok)
- [x] FS-Base per Thread (save/restore in sched_preempt)
- [x] getrandom CSPRNG (RDRAND — aktuell disabled wegen CPUID-Bug)
- [x] Futex Blocking (THREAD_BLOCKED + Waitqueue)
- [x] VFS/ramfs (open/write/close/stat/lseek/dup2/getcwd)
- [x] fork/exec/waitpid
- [x] Process Cleanup (proc_cleanup)
- [x] mmap/munmap Overflow-Check
- [x] ELF-Loader Failure-Cleanup (goto-pattern)
- [x] Networking (E1000 + TCP/IP + Socket Syscalls)
- [x] sti Race Fix (syscall_entry.asm — 61762fa)
- [x] pages_alloc/pages_free Locking
- [x] 5 HW Primitives als Syscalls (512-518)
- [x] net_port Userspace-Treiber-Bridge (SYS_COSMO_NIC_ATTACH 519)
- [x] NIC-Abstraktion (nic_driver_t, net_nic_register)
- [x] Treiber-Verzeichnis (src/drivers/ mit sauberer hw.h Trennung)
- [x] Per-Core Run Queues (32 prio × 64 cores)
- [x] Dynamische RT Core-Isolation (sched_rebalance)
- [x] INIT/SIPI AP Trampoline (16→32→64)
- [x] clone() + Preemptive Multi-Threading
- [x] Higher-Half Kernel (128TB Userspace)
- [x] VMA AVL-Baum + Demand Paging + ASLR
- [x] SSE2/AVX2 memops (MOVNTDQ, ERMS)
- [x] Kernel Benchmark (make bench)
- [x] Hardware Test (make test-hw — 27 PASS)
