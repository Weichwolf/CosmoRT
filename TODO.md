# CosmoRT — Offene Punkte

Stand: 2026-03-21. ~28K LOC, 123 Syscalls.
ktest: 61 PASS, 0 FAIL. Kein Polling (IRQ-driven).
VT interaktiv (Bernstein-Palette, Hack Font, Tippen funktioniert).

---

## Naechste Schritte (Prioritaet)

```
Prio 1 — Restrukturierung src/kernel/:
  syscall.c zerlegen             → 9 Dateien nach Subsystem (~300 Zeilen/Datei)
  process.c zerlegen             → fork.c, exec.c, thread.c, signal.c
  epoll.c zerlegen               → epoll.c, eventfd.c, timerfd.c, signalfd.c, inotify.c
  net.c zerlegen                 → tcp.c, udp.c, arp.c, icmp.c
  vfs.c zerlegen                 → vfs.c, ramfs.c
  *_bin.h nach gen/              → Generierte Dateien aus Arbeitsverzeichnis raus
  Subdirectories                 → core/ mm/ proc/ syscall/ ipc/ fs/ net/ event/ vt/ hw/

Prio 2 — Notebook-tauglich:
  P16 /dev/msr + /dev/port       → Device-Nodes
  P16 powerd + Shutdown           → HWP, Temperatur, ACPI
  P10 Code-Signing                → Ed25519 Vertrauenskette

Prio 3 — Vollstaendigkeit:
  In-Kernel Treiber entfernen     → e1000, virtio-* nur noch als Userspace-Treiber
  virtio-console/snd/fs           → Volle QEMU-Unterstuetzung
  IPv6 SLAAC                      → Dual-Stack Networking
  State-Transfer (P8)             → Graceful Driver Restart
  Link-Local 169.254.x.x          → Fallback ohne DHCP
```

---

## Offene Features

### P8 — Userspace-Treiber
- [x] e1000d, svcmgr, net_port Ring-IPC, SYS_COSMO_NIC_ATTACH
- [ ] State-Transfer-Protokoll fuer Graceful Restart
- [ ] In-Kernel e1000 entfernen (src/drivers/net/e1000.c), nur e1000d nutzen
- [ ] In-Kernel virtio-net/blk/gpu/input → Userspace-Treiber ueber net_port/hw-Primitives

### P10 — Code-Signing
- [ ] Ed25519 Owner-Key, .cosmo_sig ELF-Section, Pruefung bei execve/dlopen/kexec

### P11 — Netzwerk Zero-Config
- [x] DHCP Client, mDNS Responder
- [ ] IPv6 SLAAC
- [ ] Link-Local 169.254.x.x Fallback

### P12 — Virtio Transport
- [x] Gemeinsamer Transport, virtio-blk, virtio-net, virtio-gpu, virtio-input
- [ ] virtio-console, virtio-snd (Audio RT), virtio-fs (Host-FS)

### P16 — Power Management + ACPI
- [ ] /dev/msr, /dev/port Device-Nodes
- [ ] powerd, acpid, batteryd, backlightd, Shutdown/Reboot (Userspace)

---

## Erledigt

### Audit 2 — 2026-03-21 (30/30)

- [x] **SEC-CRIT** do_poll TOCTOU: Kernel-Stack Bounce fuer pollfd[]
- [x] **SEC-CRIT** do_sendto TOCTOU: Kernel-Bounce-Buffer vor NIC DMA
- [x] **SEC-HIGH** do_ioctl TIOCGWINSZ: Kernel-Stack struct + kmemcpy
- [x] **SEC-HIGH** do_rt_sigaction: Kernel-Stack Copy rein/raus
- [x] **SEC-HIGH** inotify_add_watch: copy_path_from_user mit Bounds-Check
- [x] **SEC-HIGH** vfs_read/vfs_write: 4KB Kernel-Bounce-Buffer
- [x] **SEC-MED** HW-Primitive: map_user_page statt Direct-Map Pointer
- [x] **SEC-MED** copy_path_from_user: ensure_user_page fuer Demand-Paging
- [x] **SEC-MED** Signal-Trampoline: dedizierte PROT_EXEC Page statt NX-Stack
- [x] **SEC-MED** DHCP XID: random_get() statt 0xDEADBEEF
- [x] **SEC-MED** DNS txid/source-port: random_get() statt timer_ms()
- [x] **CORR-HIGH** do_brk Shrink: PTE-Walk + page_free + TLB Flush
- [x] **CORR-HIGH** do_fork: IPI an RUNNING Threads auf anderen Cores
- [x] **CORR-HIGH** vfs_file refcount: __sync_fetch_and_add/sub_and_fetch
- [x] **CORR-HIGH** do_dup2/dup3: Refcount-Increment fuer alle FD-Typen
- [x] **CORR-HIGH** do_fork: Generischer Refcount fuer alle FD-backed Objects
- [x] **CORR-MED** proc_find: O(1) pid_table + __atomic_load_n auf State
- [x] **CORR-MED** build_user_stack: str_off Bounds-Check
- [x] **CORR-MED** execve Stack: str_off Bounds-Check
- [x] **CORR-MED** do_wait4: Kernel-Variable + kmemcpy
- [x] **CORR-MED** random_add_interrupt_entropy: rng_lock
- [x] **CORR-MED** pipe_slab_ensure: CAS 3-State Init
- [x] **CORR-MED** sig_pending/sig_blocked: __atomic_fetch_or/and
- [x] **PERF-HIGH** proc_find: O(1) via pid_table[256]
- [x] **PERF-HIGH** find_thread_by_tid: O(1) via tid_table[512]
- [x] **PERF-MED** epoll_wait: Lock-Scope reduziert (Copy+Release+Scan)
- [x] **PERF-MED** do_munmap: vma_find_overlap AVL-Walk statt Page-by-Page
- [x] **PERF-MED** do_mmap MAP_FIXED: vma_find_overlap AVL-Walk
- [x] **ARCH** Kernel-Mode #PF Handler fuer Demand-Paging auf User-Memory
- [x] **ARCH** FD Refcounting: Generisch fuer alle FD-Typen (fork/dup/close)
- [x] **ARCH** Netzwerk: Per-Queue Spinlock (5 Queues entkoppelt)

### Audit 1 — SEC-HIGH (6/6)

- [x] pages_alloc/pages_free Locking (Buddy-Allocator mit spin_lock_irq)
- [x] HW-Primitives Capability (HW_CAP_CHECK Macro, is_driver Flag)
- [x] TOCTOU auf User-Pointern (kmemcpy in Kernel-Buffer vor Nutzung)
- [x] kexec user_ok (user_ok + kmemcpy in kbuf)
- [x] Signal-Frame VMA-Check (vma_find + PROT_WRITE + ensure_user_page)
- [x] ELF e_phentsize validiert (>= sizeof(Elf64_Phdr) in elf_load/elf_load_ex)

### Audit 1 — PERF-HIGH (3/3)

- [x] page_alloc O(1) (Buddy-Allocator mit Free-Lists, Orders 0-9)
- [x] pages_alloc O(1) (Buddy order_for_pages → buddy_alloc_order)
- [x] futex_lock_pi blockiert (Wait-Queue, kein Spin)

### Audit 1 — SEC-MED + PERF-MED (10/10)

- [x] proc_cleanup: FD_SOCKET/FD_PIPE freigeben (fd_cleanup_entry)
- [x] sched_setparam: Priority-Bounds 0..31, -EINVAL bei Verletzung
- [x] net_http_get: Bounds-Check ri < 510, Return -1 bei Overflow
- [x] e1000_send: Timeout nach 1000 Iterationen statt Endlos-Spin
- [x] slab_free: Pool-Membership + Alignment-Check
- [x] vma_find_free: AVL-Traversal statt 16KB Stack-Array (512B)
- [x] fork: Parent-Threads gestoppt (THREAD_BLOCKED + saved_priority=-2)
- [x] sched_rebalance: bounded O(ncores), nur 1x/s, skip bei <4 Cores
- [x] percpu_self: GS-basiert (self-Pointer bei gs:40, ~3 Zyklen statt ~100)
- [x] IPC: per-Endpoint Lock (globaler Lock nur noch fuer Allokation)

### P0 — Sofort (Silent Corruption / Crash)
- [x] sched_yield, VFS read #GP, getrandom RDRAND, fork Race, pages Locking, sti

### P1 — Sicherheit (Exploitable)
- [x] NX-Bit W^X, ChaCha20 CSPRNG, ASLR, copy_path_from_user, user_ok Fixes

### P2 — Korrektheit
- [x] TLB Shootdown, fork FD refcount, clone Thread-Liste, cwd, execve argv, Signale, IPC

### P3 — Fehlende Features
- [x] pipe/pipe2, mkdir/rmdir/unlink/rename, getdents64, ioctl/fcntl, readv/writev

### P4 — Robustheit
- [x] net_poll SMP-safe, sock_alloc Lock, TCP CSPRNG, do_write, Packet-Queues, Idle-Stacks

### P5 — CosmoFS + virtio-blk
- [x] virtio-blk, Block-Cache LRU, B+ Tree, Journal WAL, CosmoFS, VFS Mount, mkfs, disk.img

### P6 — Signale
- [x] k_sigaction, deliver_signal, sigreturn, SA_RESTORER, Signal-Blocking, 13 Tests

### P7 — Dynamischer Linker
- [x] PT_INTERP, ld-cosmo.so, DT_HASH, Relocations, Auxvec
- [x] dlopen/dlsym/dlclose/dlerror (Runtime-Loading, 16 Handles)

### P9 — kexec
- [x] ELF-Validation, bcache/journal Flush, AP Stop, Identity-Trampoline, SYS_COSMO_KEXEC

### P13 — Hyper-V
- [x] Detection, Hypercall Page, Reference TSC, VMBus, storvsc, netvsc, hyperv_fb, hv_kbd, hv_mouse, hv_utils

### P14 — Virtual Terminals
- [x] PTY, VT-Buffer, ANSI-Parser, Font-Renderer, Framebuffer
- [x] VT-Switch, Keyboard-Input, Scancode-Mapping, Echo-Flush, Blocking PTY read
- [x] Hack Font 1546 Glyphen, BMW Bernstein Farbpalette

### P15 — Syscalls fuer CosmoCL
- [x] DUP3, PIPE, SYSINFO, GETRUSAGE, PRLIMIT64, TIMES
- [x] FCHMOD, FCHOWN, SYMLINK, READLINK, TRUNCATE, FTRUNCATE
- [x] LSTAT, MKNODAT, FCHMODAT, FSTATAT, UTIMENSAT, FALLOCATE, Symlinks
- [x] EPOLL, EVENTFD, TIMERFD, SIGNALFD (Stub)
- [x] INOTIFY mit echten Events (IN_CREATE/DELETE/MODIFY/MOVED, 32-Entry Ring)

### procfs
- [x] /proc/dmesg, /proc/meminfo, /proc/cpuinfo
