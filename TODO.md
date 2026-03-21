# CosmoRT — Offene Punkte

Stand: 2026-03-21.

ktest Ergebnis: 38 PASS, 0 FAIL, 0 SKIP. (P3+P4 erledigt)

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

### ~~Kein TLB Shootdown~~
Erledigt: IPI-basierter TLB Shootdown (vector 0xFE). munmap, mprotect,
free_address_space senden IPI an alle Cores mit gleichem PML4.

### ~~fork FD-Sharing ohne Refcount~~
Erledigt: vfs_file hat refcount. fork inkrementiert, close dekrementiert.
Nur bei refcount==0 wird freigegeben.

### ~~do_clone Thread-Liste ohne Lock~~
Erledigt: spin_lock_irq(&proc->lock) um Thread-Listen-Mutation in do_clone.

### ~~cwd ist global statt per-Process~~
Erledigt: cwd in process_t verschoben. fork kopiert, vfs_getcwd/vfs_chdir
nutzen proc_current()->cwd.

### ~~execve ignoriert argv/envp~~
Erledigt: argv/envp werden vor free_address_space kopiert, Stack wird
nach elf_load mit echten Strings/Pointern (Linux ABI) aufgebaut.

### ~~Signal-Delivery an Userspace fehlt~~
Erledigt (minimal): check_pending_signals() in INT 0x80 und sched_preempt.
SIG_DFL: fatale Signale (KILL/SEGV/PIPE/TERM/ABRT) terminieren Prozess.
SIG_IGN: ignoriert. User-Handler: TODO (braucht Signal-Frame + sigreturn).

### ~~IPC blocking/waking nicht verbunden~~
Erledigt: ipc_recv spinnt kurz, returnt -EAGAIN. ipc_send/ipc_notify
wecken blockierte Threads via blocked_tid + sched_add.

---

## ~~P3 — Fehlende Features (Bootstrap-Blocker)~~

Erledigt.

- ~~pipe/pipe2 (Shell-Pipelines)~~
- ~~mkdir/rmdir/unlink/rename (Dateisystem-Mutation)~~
- ~~getdents64 (ls, find, readdir)~~
- ~~ioctl/fcntl (Terminal-Control, FD-Flags)~~
- ~~readv (CosmoPX stdio)~~

---

## ~~P4 — Robustheit~~

Erledigt.

- ~~net_poll static Buffer (SMP Packet-Korruption)~~
- ~~sock_alloc ohne Lock (concurrent socket())~~
- ~~TCP Sequence/Port vorhersagbar (CSPRNG statt inkrementell)~~
- ~~do_write returnt falsche Laenge (64KB Limit, returnt count)~~
- ~~Packet-Queues ohne Lock (SMP)~~
- ~~Idle-Stacks 8KB zu klein (16KB jetzt)~~
- ~~Spinlock CAS statt Load-Wait (atomic load statt LOCK CMPXCHG)~~

---

## P5 — CosmoFS + virtio-blk (Persistentes Dateisystem)

/tmp und / muessen auf Disk liegen. ramfs reicht nur fuer Bootstrap.
CosmoFS: eigenes Dateisystem, inspiriert von BeOS BFS. Kein ext2-Port,
kein "minimal erstmal" — gleich richtig.

### Design

```
Superblock (Block 0, 4KB):
  magic: "CosmoFS\0"
  version: 1
  block_size: 4096
  total_blocks: uint64_t
  free_blocks: uint64_t
  bitmap_start: uint64_t      (Block-Nummer der Free-Bitmap)
  root_inode: uint64_t        (Block-Nummer des Root-Inode)
  journal_start: uint64_t     (Block-Nummer des Journal-Rings)
  journal_size: uint32_t      (Bloecke)

Inode (256 Bytes, 16 pro Block):
  type: uint16_t              (file, dir, symlink, device)
  flags: uint16_t
  uid, gid: uint32_t          (immer 0 fuer Single-User)
  size: uint64_t
  blocks_used: uint64_t
  ctime, mtime, atime: uint64_t  (Nanosekunden seit Epoch)
  direct[12]: uint64_t        (12 × 4KB = 48KB direkt)
  indirect: uint64_t          (4KB/8 = 512 Eintraege → 2MB)
  double_indirect: uint64_t   (512 × 512 = 262144 → 1GB)
  triple_indirect: uint64_t   (512^3 → 512GB)
  attr_block: uint64_t        (B+ Tree Root fuer Attribute)
  stream_block: uint64_t      (benannte Neben-Streams)
  reserved[4]: uint64_t

Directory-Eintraege (B+ Tree):
  Sortiert nach Name. Leaf enthalt (name, inode_number).
  O(log n) Lookup fuer Millionen Dateien.

Journal (Metadata-only, Write-Ahead):
  Ring-Buffer: [Transaction-Header][Metadata-Blocks][Commit-Record]
  Bei Crash: Replay uncommitted Transactions → konsistent.
  Kein fsck. Kein Datenverlust bei Metadata.

Attribute (pro Datei, B+ Tree):
  Beliebige Key-Value-Paare:
    "audio:artist" = "Bach"
    "email:from" = "cosmo@example.com"
    "mime:type" = "text/plain"
  Index ueber Attribute → Queries:
    cosmo_query("mime:type == audio/* AND size > 1MB")

Streams (pro Datei):
  Haupt-Stream: normaler Datei-Inhalt (read/write)
  Benannte Streams: Thumbnails, Previews, Caches
    open("/photo.jpg:thumbnail") → Stream-Zugriff
```

### Komponenten

1. **virtio-blk Treiber** (src/drivers/blk/virtio_blk.c)
   Ueber hw.h Primitives (cosmo_mmio_map, cosmo_dma_alloc, cosmo_irq_register).
   API: blk_read(block_nr, buf), blk_write(block_nr, buf).
   Existiert bereits in ~/Git/llmos/src/drivers/block/virtio_blk.c als Referenz.

2. **Block-Cache** (src/kernel/bcache.c)
   LRU Cache fuer Disk-Blocks im RAM. Write-Back mit Journal-Schutz.
   Reduziert I/O: hot Blocks (Superblock, Bitmap, Directory) bleiben im RAM.

3. **B+ Tree** (src/kernel/btree.c)
   Generisch: fuer Directories UND Attribute-Indices.
   Insert, Delete, Search, Range-Query. Persistent auf Disk-Blocks.

4. **Journal** (src/kernel/journal.c)
   Write-Ahead-Log fuer Metadata-Operationen.
   Transaction: begin → write metadata blocks → commit.
   Recovery: bei Boot pruefen, uncommitted Transactions replaying.

5. **CosmoFS** (src/kernel/cosmofs.c)
   Superblock lesen, Inode lesen/schreiben, Block allozieren/freigeben,
   Directory-Operationen (lookup, create, delete, rename).
   Attribute-Operationen (get, set, remove, query).

6. **VFS Mount-Layer** (src/kernel/vfs.c erweitern)
   Mount-Table: Pfad-Prefix → FS-Backend.
   "/" → CosmoFS auf virtio-blk
   "/dev/shm" → ramfs (bestehend)
   Dispatch in vfs_open/read/write nach Mount-Point.

7. **mkfs.cosmo** (tools/mkfs.c)
   Host-Tool: erstellt CosmoFS-Image fuer QEMU.
   Schreibt Superblock, Bitmap, Root-Directory, Journal.

### CosmoLib-API (nicht POSIX-gebunden)

```c
// Benannte Attribute (BeOS-Stil)
int cosmo_attr_set(int fd, const char *name, const void *val, size_t len);
int cosmo_attr_get(int fd, const char *name, void *val, size_t len);
int cosmo_attr_remove(int fd, const char *name);
int cosmo_attr_list(int fd, char *buf, size_t bufsize);

// Attribut-Queries (Datenbank-artig)
int cosmo_query_open(const char *query);  // "type==audio AND size>1MB"
int cosmo_query_next(int qfd, char *path, size_t pathlen);
void cosmo_query_close(int qfd);

// Benannte Streams (NTFS-artig)
int cosmo_stream_open(int fd, const char *stream_name, int flags);
```

### Reihenfolge

```
5.1 virtio-blk Treiber (src/drivers/blk/)     — Referenz in llmos
5.2 Block-Cache (LRU, Write-Back)              — 200-300 Zeilen
5.3 B+ Tree (generisch, persistent)            — 400-600 Zeilen
5.4 Journal (WAL, Ring-Buffer)                  — 200-300 Zeilen
5.5 CosmoFS Core (Superblock, Inode, Bitmap)   — 400-600 Zeilen
5.6 CosmoFS Dirs (B+ Tree Directories)         — 200-300 Zeilen
5.7 CosmoFS Attrs (B+ Tree Attributes)         — 200-300 Zeilen
5.8 VFS Mount-Layer                             — 100-200 Zeilen
5.9 mkfs.cosmo (Host-Tool)                     — 200-300 Zeilen
5.10 /tmp auf CosmoFS, /dev/shm auf ramfs
```

Geschaetzte Groesse: ~2500-3500 Zeilen. Verdoppelt den Kernel fast.
Aber: das ist THE Feature das CosmoOS von einem Toy-OS unterscheidet.

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
- [x] TLB Shootdown (IPI vector 0xFE, munmap/mprotect/free_address_space)
- [x] fork FD refcount (vfs_file.refcount)
- [x] do_clone Thread-Liste unter proc->lock
- [x] cwd per-Process (process_t.cwd)
- [x] execve argv/envp (kopieren + Stack-Rebuild)
- [x] Signal-Delivery minimal (SIG_DFL/SIG_IGN in INT 0x80 + preempt)
- [x] IPC blocking/waking (spin-yield + blocked_tid wake)
- [x] Boot-Filesystem: /tmp, /dev, /proc, /bin, /etc bei vfs_init
- [x] pipe/pipe2 (SYS_PIPE2 293, FD_PIPE, PIPE_BUF_SIZE 4096)
- [x] mkdir/rmdir/unlink/rename (SYS_MKDIR 83, RMDIR 84, UNLINK 87, RENAME 82)
- [x] mkdirat/unlinkat/renameat2 (258, 263, 316)
- [x] getdents64 (SYS_GETDENTS64 217, linux_dirent64)
- [x] ioctl (SYS_IOCTL 16, TIOCGWINSZ 80x24)
- [x] fcntl (SYS_FCNTL 72, F_GETFL/F_SETFL/F_DUPFD/F_GETFD/F_SETFD)
- [x] readv (SYS_READV 19, scatter read)
- [x] writev generalisiert (alle FD-Typen, nicht nur Serial)
- [x] net_poll Stack-lokaler Buffer (SMP-safe)
- [x] sock_alloc Spinlock
- [x] TCP Seq/Port CSPRNG (random_get statt inkrementell)
- [x] do_write returnt actual statt count
- [x] Packet-Queues Spinlock (net_q_lock)
- [x] Idle-Stacks 16KB (war 8KB)
- [x] Spinlock atomic load statt CAS (kein LOCK CMPXCHG im Spin-Loop)
