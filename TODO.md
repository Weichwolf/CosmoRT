# CosmoRT — Offene Punkte

Stand: 2026-03-21. ~22K LOC, 100 Quelldateien, 95 Syscalls.
ktest: 51 PASS, 0 FAIL. Kein Polling (IRQ-driven).

---

## Audit-Ergebnisse

### SEC-HIGH (6/6 gefixt)

- [x] pages_alloc/pages_free Locking (Buddy-Allocator mit spin_lock_irq)
- [x] HW-Primitives Capability (HW_CAP_CHECK Macro, is_driver Flag)
- [x] TOCTOU auf User-Pointern (kmemcpy in Kernel-Buffer vor Nutzung)
- [x] kexec user_ok (user_ok + kmemcpy in kbuf)
- [x] Signal-Frame VMA-Check (vma_find + PROT_WRITE + ensure_user_page)
- [x] ELF e_phentsize validiert (>= sizeof(Elf64_Phdr) in elf_load/elf_load_ex)

### PERF-HIGH (3/3 gefixt)

- [x] page_alloc O(1) (Buddy-Allocator mit Free-Lists, Orders 0-9)
- [x] pages_alloc O(1) (Buddy order_for_pages → buddy_alloc_order)
- [x] futex_lock_pi blockiert (Wait-Queue, kein Spin)

### SEC-MED + PERF-MED (10/10 gefixt)

- [x] proc_cleanup: FD_SOCKET/FD_PIPE freigeben (fd_cleanup_entry)
- [x] sched_setparam: Priority-Bounds 0..31, -EINVAL bei Verletzung
- [x] net_http_get: Bounds-Check ri < 510, Return -1 bei Overflow
- [x] e1000_send: Timeout nach 1000 Iterationen statt Endlos-Spin
- [x] slab_free: Pool-Membership + Alignment-Check
- [x] vma_find_free: AVL-Traversal statt 16KB Stack-Array (512B)

Akzeptabel (kein Fix noetig):
- [x] fork: Parent-Threads gestoppt (THREAD_BLOCKED + saved_priority=-2)
- [x] sched_rebalance: bounded O(ncores), nur 1x/s, skip bei <4 Cores
- IPC globaler Lock: akzeptabel bei IPC_MAX_ENDPOINTS=64
- percpu_self LAPIC MMIO: unvermeidbar ohne rdgsbase, akzeptabel

---

## Erledigt

### P0 — Sofort (Silent Corruption / Crash)
- [x] sched_yield: hint-only return 0 (Timer-Preemption reicht)
- [x] VFS read #GP: ELF-Loader Stack-Alignment (16n+8 statt 16n)
- [x] getrandom: RDRAND-Erkennung in memops_init (NASM)
- [x] fork Race: Parent-Threads stoppen waehrend copy_address_space
- [x] pages_alloc/pages_free Locking
- [x] sti nach Frame-Save verschoben

### P1 — Sicherheit (Exploitable)
- [x] map_user_page mit Protection-Flags (NX-Bit, W^X, EFER.NXE)
- [x] ChaCha20 CSPRNG (random.c — 6 Entropie-Quellen + RDRAND)
- [x] ASLR via CSPRNG statt RDTSC
- [x] copy_path_from_user (String-Boundary-Check)
- [x] do_clock_getres + do_wait4 user_ok Fixes

### P2 — Korrektheit (Funktional kaputt)
- [x] TLB Shootdown (IPI vector 0xFE)
- [x] fork FD refcount (vfs_file.refcount)
- [x] do_clone Thread-Liste unter proc->lock
- [x] cwd per-Process (process_t.cwd)
- [x] execve argv/envp (kopieren + Stack-Rebuild)
- [x] Signal-Delivery minimal (SIG_DFL/SIG_IGN)
- [x] IPC blocking/waking

### P3 — Fehlende Features (Bootstrap-Blocker)
- [x] pipe/pipe2, mkdir/rmdir/unlink/rename
- [x] getdents64, ioctl/fcntl, readv/writev

### P4 — Robustheit
- [x] net_poll Stack-lokaler Buffer (SMP-safe)
- [x] sock_alloc Spinlock
- [x] TCP Seq/Port CSPRNG
- [x] do_write returnt actual statt count
- [x] Packet-Queues Spinlock
- [x] Idle-Stacks 16KB, Spinlock atomic load

### P5 — CosmoFS + virtio-blk
- [x] virtio-blk Treiber
- [x] Block-Cache LRU (bcache.c)
- [x] B+ Tree generisch (btree.c)
- [x] Journal WAL (journal.c)
- [x] CosmoFS Core (cosmofs.c)
- [x] VFS Mount-Layer
- [x] mkfs.cosmo (tools/mkfs.c)
- [x] disk.img 64MB fuer QEMU

### P6 — Signal User-Handler + sigreturn
- [x] k_sigaction struct, deliver_signal, SYS_RT_SIGRETURN
- [x] SA_RESTORER + On-stack Trampoline Fallback
- [x] Signal-Delivery in SYSCALL/INT80/Timer-Preempt-Path
- [x] Signal-Blocking (sa_mask + auto-block + sigprocmask)
- [x] Fork erbt sig_actions + sig_blocked
- [x] 13 Signal-Tests in ktest

### P7 — Dynamischer Linker (ld-cosmo.so)
- [x] PT_INTERP Erkennung in elf.c
- [x] ld-cosmo.so: ELF64 Parsing, PT_LOAD, PT_DYNAMIC
- [x] Symbol-Resolution via DT_HASH
- [x] Relocations: R_X86_64_RELATIVE, GLOB_DAT, JUMP_SLOT
- [x] Auxiliary Vector (AT_PHDR, AT_ENTRY, AT_BASE, etc.)
- [ ] dlopen/dlsym/dlclose (Runtime-Loading, noch nicht implementiert)

### P8 — Userspace-Treiber + Service-Manager
- [x] e1000d (src/user/e1000d.c, 540 Zeilen)
- [x] svcmgr (src/user/svcmgr.c, 156 Zeilen)
- [x] net_port Shared-Memory Ring-IPC (src/kernel/net_port.c)
- [x] SYS_COSMO_NIC_ATTACH (519)
- [ ] State-Transfer-Protokoll fuer Graceful Restart

### P9 — kexec (Kernel Hot-Swap)
- [x] ELF-Validation, bcache/journal Flush
- [x] AP Stop via INIT IPI
- [x] Identity-mapped Trampoline bei 0x8000
- [x] SYS_COSMO_KEXEC (520)

### P11 — Netzwerk Zero-Config (teilweise)
- [x] DHCP Client (Discover/Request/ACK)
- [x] mDNS Responder (cosmo-XXXX.local)
- [ ] IPv6 SLAAC (nicht implementiert)
- [ ] Link-Local 169.254.x.x Fallback

### P12 — Virtio Transport + Treiber (teilweise)
- [x] Gemeinsamer Transport (src/drivers/virtio/virtio.c)
- [x] virtio-blk
- [x] virtio-net
- [x] virtio-gpu (2D Framebuffer)
- [x] virtio-input (Tastatur + Maus)
- [ ] virtio-console
- [ ] virtio-snd (Audio RT)
- [ ] virtio-fs (Host-FS Sharing)

### P13 — Hyper-V Support
- [x] Detection (CPUID 0x40000000), MSR Setup, SynIC
- [x] Hypercall Page (HvPostMessage, HvSignalEvent)
- [x] Reference TSC
- [x] VMBus (Version Negotiation, Channel Offers, GPADL, Ring Buffers)
- [x] storvsc (SCSI Block I/O)
- [x] netvsc (RNDIS, registriert als nic_driver_t)
- [x] hyperv_fb (Synthvid, Stub)
- [x] hv_kbd + hv_mouse (Stubs)
- [x] hv_utils (Heartbeat, Shutdown, Time Sync)

---

## In Arbeit

### procfs — Virtuelles Filesystem (/proc)

Ring-Buffer (64KB) in serial.c, jeder serial_putchar → COM1 + Ring.
procfs generiert Inhalt on-the-fly aus Kernel-Daten.

- [ ] /proc/dmesg — Kernel-Log (liest aus Ring-Buffer)
- [ ] /proc/meminfo — Buddy-Allocator Stats
- [ ] /proc/cpuinfo — Cores, TSC-Frequenz

Kein Disk-I/O, kein /tmp/dmesg. `cat /proc/dmesg` liest direkt.

---

## Offen

### P10 — Code-Signing (Vertrauenskette)

Nicht angefangen. Ed25519 Owner-Key, .cosmo_sig ELF-Section.
Pruefung bei execve/dlopen/kexec.

### P14 — Virtual Terminals (PTY + VT)

Nicht angefangen. Braucht: PTY, VT-Buffer, Font-Renderer,
ANSI-Emulation, VT-Switch (Ctrl+Alt+F1-F4).
Abhaengig von: virtio-input oder hv_kbd fuer Keyboard-Events.

### P15 — Fehlende Syscalls fuer CosmoCL

```
Haben (95):  read write open close stat fstat lseek mmap munmap
             mprotect brk clone fork execve wait4 kill pipe2 dup2
             getcwd chdir mkdir mkdirat rmdir unlink unlinkat rename
             renameat2 getdents64 ioctl fcntl readv writev poll
             socket connect bind listen accept sendto recvfrom
             sendmsg recvmsg getsockname getpeername setsockopt
             getsockopt shutdown socketpair futex clock_gettime
             clock_getres clock_nanosleep nanosleep gettimeofday
             getpid getppid gettid getuid geteuid getgid getegid
             uname access openat getrandom arch_prctl rt_sigaction
             rt_sigprocmask rt_sigreturn sched_yield sched_getscheduler
             sched_setscheduler sched_getparam sched_setparam
             sched_getaffinity sched_setaffinity set_tid_address
             prlimit64 set_robust_list rseq mlock mlockall munlock
             munlockall
             + 9 CosmoRT-spezifische (512-520)

Fehlen (gruppiert nach Bedarf):

  Filesystem-Metadata:
    FCHMOD FCHOWN LINK SYMLINK READLINK TRUNCATE FTRUNCATE
    LSTAT MKNODAT FCHMODAT FSTATAT UTIMENSAT FALLOCATE

  Event-APIs (Node.js/libuv):
    EPOLL_CREATE1 EPOLL_CTL EPOLL_WAIT
    EVENTFD SIGNALFD TIMERFD_CREATE TIMERFD_SETTIME
    INOTIFY_INIT1 INOTIFY_ADD_WATCH INOTIFY_RM_WATCH

  Misc:
    DUP3 PIPE SYSINFO GETRUSAGE SETRLIMIT TIMES
```

Fuer "Hello World" + printf: reicht.
Fuer Ruby/Python: + Filesystem-Metadata.
Fuer Node.js: + EPOLL + EVENTFD + TIMERFD + INOTIFY.
Fuer Claude Code: alles oben.

---

## Naechste Schritte (Prioritaet)

```
Prio 1 — Interaktives CosmoOS:
  procfs                        → /proc/dmesg, /proc/meminfo, /proc/cpuinfo
  P14 Virtual Terminals + PTY   → Bash-Prompt auf Framebuffer
  Framebuffer earlycon          → Boot-Messages auf Bildschirm (kein COM1 noetig)
  P15 Filesystem-Metadata       → SYMLINK, READLINK, TRUNCATE, CHMOD
  dlopen/dlsym                  → Runtime Library Loading

Prio 2 — Node.js / Claude Code:
  EPOLL + EVENTFD + TIMERFD     → Async I/O (libuv braucht das)
  INOTIFY                       → File-Watching
  P10 Code-Signing              → Vertrauenskette (Ed25519)

Prio 3 — Vollstaendigkeit:
  virtio-console/snd/fs         → Volle QEMU-Unterstuetzung
  IPv6 SLAAC                    → Dual-Stack Networking
  State-Transfer (P8)           → Graceful Driver Restart
```
