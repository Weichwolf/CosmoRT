# CosmoRT — Offene Punkte

Stand: 2026-03-21.

ktest Ergebnis: 38 PASS, 0 FAIL, 0 SKIP.

---

## P0 — Sofort (Silent Corruption / Crash)

### ~~sched_yield Kernel NULL-Deref~~
Erledigt: `do_sched_yield` gibt jetzt `return 0` zurueck (hint-only).
Timer-Preemption uebernimmt das eigentliche Context-Switching.

### ~~VFS read #GP~~
Erledigt: Root Cause war Stack-Misalignment. ELF-Loader setzte RSP
auf 16n, GCC-kompiliertes `_start` erwartet 16n+8 (wie nach CALL).
`movaps` auf misaligned Stack → #GP. Fix: `sp = (sp & ~0xF) - 8`.

### ~~getrandom CPUID #GP~~
Erledigt: RDRAND-Erkennung nach memops_init (NASM) verschoben.
CPUID in PIC-kompiliertem C war das Problem. `memops_has_rdrand`
wird in memops.asm gesetzt, syscall.c liest die Variable.

### ~~fork Race bei Page-Copy~~
Erledigt (Short-Term): Parent-Threads werden vor copy_address_space
gestoppt (RUNNABLE → BLOCKED mit saved_priority=-2 als Marker) und
danach wieder aufgeweckt. Langfristig: COW.

### ~~pages_alloc/pages_free ohne Lock~~
Erledigt: pages_alloc/pages_free haben seit Refactor Spinlocks.

### ~~sti vor Frame-Completion~~
Erledigt: sti nach Frame-Save verschoben (61762fa).

---

## P1 — Sicherheit (Exploitable)

### ~~map_user_page ignoriert Protection-Flags~~
Erledigt: prot Parameter, prot_to_pte(), NX-Bit via EFER.NXE.
W^X enforced. AP Cores bekommen NXE via syscall_init() in ap_main.

### ~~ASLR mit RDTSC~~
Erledigt: `aslr_rand()` nutzt jetzt ChaCha20 CSPRNG (random.c).

### ~~user_ok(path, 1) prueft nur erstes Byte~~
Erledigt: copy_path_from_user() kopiert Byte-fuer-Byte mit Grenz-Check.

### ~~do_clock_getres ohne user_ok~~
Erledigt: user_ok Check hinzugefuegt.

### ~~do_wait4 user_ok unvollstaendig~~
Erledigt: Overflow-safe Validation mit user_ok(addr, sizeof(int)).

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
- [x] Hardware Test (make test-hw — 36 PASS)
- [x] sched_yield: hint-only return 0 (Timer-Preemption reicht)
- [x] VFS read #GP: ELF-Loader Stack-Alignment (16n+8 statt 16n)
- [x] getrandom: RDRAND-Erkennung in memops_init (NASM)
- [x] fork Race: Parent-Threads stoppen waehrend copy_address_space
- [x] ChaCha20 CSPRNG (random.c — kein RDRAND noetig, 6 Entropie-Quellen)
- [x] ASLR via CSPRNG statt RDTSC
- [x] getrandom immer verfuegbar (38 PASS, 0 SKIP)
- [x] map_user_page mit Protection-Flags (NX-Bit, W^X, EFER.NXE)
- [x] copy_path_from_user (String-Boundary-Check statt user_ok(path,1))
- [x] do_clock_getres + do_wait4 user_ok Fixes
