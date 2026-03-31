# CosmoRT — Offene Punkte

Stand: ktest 1235/0, musl 452/20, LTP 11/87 (105/313 erreicht)

## Lock-Granularitaet: Globale Locks → Linux-Vorbild

Globale Locks blockieren alle Cores — inkompatibel mit RT. Linux-Vorbild: per-CPU, per-Object, per-Zone.

- [ ] **Per-CPU Page Lists** (page_alloc.c): `buddy_lock` global → per-CPU Freelists mit Batch-Refill. Hot-Path (alloc/free) wird lock-free. `zone->lock` nur fuer Refill wenn per-CPU-Liste leer. Vorbild: Linux `struct per_cpu_pages`, Batch 31.
- [ ] **Per-Inode Lock** (ext2.c, vfs.c): `fs_lock` global → rw_semaphore in `vfs_node`. Parallele Reads auf verschiedenen Dateien blockieren sich nicht mehr. Vorbild: Linux `inode->i_rwsem`.
- [ ] **Per-Block Locking** (bcache.c): Globales bcache-Lock → per-Block atomare Flags oder per-Inode Granularitaet. Vorbild: Linux `bh->b_state` Bitops.
- [ ] **Per-CPU Slab** (slab.c): Globale Freelist → per-CPU partial lists. Vorbild: Linux SLUB per-CPU Caches.

## musl libc-test Fixes (20 FAIL → 0)

- [ ] socket/socket-static: accept() Gateway-Check entfernen (`socket.c:669-671` — EAGAIN für alle Sockets wenn `net_gw_ip==0`, blockiert Loopback). Musl-Test: `accept(s, ...)` auf listen-Socket liefert EAGAIN statt Connection.
- [ ] utime/utime-static (ENOTDIR): VFS Path-Resolution liefert ENOENT statt ENOTDIR für `/dev/null/invalid`. `vfs_lookup.c:58` returned 0 ohne err zu setzen wenn Kind nicht gefunden. ext2_walk→ramfs-Fallthrough verliert Fehlercode.
- [ ] utime/utime-static (Timestamps): 32-Bit-Truncation in `vfs_ioctls.c:369,393`. Werte ≥2³² (z.B. `1LL<<32`) werden zu 0. `vfs_node.atime/mtime/ctime` und ext2 `i_atime/i_mtime` sind uint32_t. Auf int64_t erweitern (ramfs) bzw. ext2 extra-time-Felder nutzen.
- [ ] sem_open/sem_open-static: Initialwert 0 statt 1. musl sem_open → shm_open + mmap(MAP_SHARED). Entweder MAP_SHARED Kohärenz kaputt (ramfs liefert unterschiedliche Pages) oder shm_open/mmap-Interaktion fehlerhaft.
- [ ] pthread_robust/pthread_robust-static: Timeout (Deadlock). Robust-Futex-Cleanup bei Thread/Process-Exit fehlt — `robust_list` wird gespeichert (`set_robust_list`) aber nie abgearbeitet. Sterbender Owner hinterlässt locked Futex → Waiter blockiert ewig.
- [ ] malloc-brk-fail-static: `malloc(10000)` gelingt nach mmap-Exhaustion+brk-Collision. brk-Fallback zu großzügig oder VMA-Overlap-Check in brk fehlt. `sys_mem.c` brk-Pfad prüfen.
- [ ] fma: Signed-Zero-Propagation bei Subnormals. `fma(-0x1p-1000, 0x1p-100, 0x0p+0)` liefert `+0` statt `-0`. Vermutlich FPU-Rounding-Mode oder Kernel clobbered FPU-State.
- [ ] fmal: FP-Exception-Flag UNDERFLOW fehlt bei bestimmten Operationen. Gleiche Ursache wie fma — FPU-State-Management prüfen.
- [ ] powf: Spurious INEXACT|OVERFLOW|UNDERFLOW bei Identity-Fällen (`powf(x, 1.0)`). FPU-State-Kontamination oder musl-Bug bei Subnormals.
- [ ] remquol: Quotient immer 0. `remquol(a,b)` liefert korrekten Remainder aber Quotient-Pointer wird nie geschrieben. Kernel-FPU oder musl-Codepath — `fprem1` Instruction?
- [ ] tls_get_new-dtv: dlopen("...dso.so") → SEGFAULT (RIP=0, CR2=0). NULL-Funktionszeiger nach fehlgeschlagenem dlopen. Dynamischer Linker (ld-musl) kann .so nicht laden — fehlende mmap-Flags, fehlende Relocation-Typen, oder TLS-Setup defekt.

## Linux-Konformität (LTP / Allgemein)

- [ ] execve: Execute-Bit prüfen (`process_exec.c:241` — `ip.i_mode & 0111`, sonst EACCES). Blockiert 60+ LTP-Tests (rc=126-Lawine ab chmod07).
- [ ] accept: ENOTSOCK statt EBADF wenn FD gültig aber kein Socket (`socket.c:617` — `sock_from_fd()` NULL-Pfad).
- [ ] accept4: Flags (a4) an `do_accept()` durchreichen (`dispatch.c:181`) und in `fd_alloc()` anwenden (`socket.c:645` — SOCK_CLOEXEC/SOCK_NONBLOCK).
- [ ] VMA/TLB Race: VMA-Update und TLB-Shootdown atomar machen (`sys_mem.c:333-337`, `vma.c:62-76` vs `irq.c:335`). SMP-Crashes bei chdir04/chmod06.
