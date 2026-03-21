# CosmoRT — Offene Punkte

Stand: 2026-03-21. ~25K LOC, 123 Syscalls.
ktest: 61 PASS, 0 FAIL. Kein Polling (IRQ-driven).

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

Erledigt:
- [x] fork: Parent-Threads gestoppt (THREAD_BLOCKED + saved_priority=-2)
- [x] sched_rebalance: bounded O(ncores), nur 1x/s, skip bei <4 Cores

- [x] percpu_self: GS-basiert (self-Pointer bei gs:40, ~3 Zyklen statt ~100)
- [x] IPC: per-Endpoint Lock (globaler Lock nur noch fuer Allokation)

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

### procfs
- [x] /proc/dmesg — Kernel-Log aus 64KB Ring-Buffer
- [x] /proc/meminfo — Buddy-Allocator Stats
- [x] /proc/cpuinfo — Cores, TSC-Frequenz

### P15 — Syscalls fuer CosmoCL (in Arbeit)

Misc (erledigt):
- [x] DUP3, PIPE, SYSINFO, GETRUSAGE, PRLIMIT64, TIMES

Filesystem-Metadata (erledigt):
- [x] FCHMOD FCHOWN SYMLINK READLINK TRUNCATE FTRUNCATE
- [x] LSTAT MKNODAT FCHMODAT FSTATAT UTIMENSAT FALLOCATE
- [x] Symlinks: COSMOFS_TYPE_SYMLINK, Target als File-Data
- [ ] LINK (hard links): -ENOSYS (CosmoFS hat keine)

Event-APIs (erledigt):
- [x] EPOLL_CREATE1 EPOLL_CTL EPOLL_WAIT (epoll.c, Slab-basiert)
- [x] EVENTFD2 (Counter-basiert, read/write)
- [x] TIMERFD_CREATE TIMERFD_SETTIME (CLOCK_MONOTONIC)
- [x] SIGNALFD4 (Stub, -ENOSYS)
- [x] INOTIFY_INIT1 INOTIFY_ADD_WATCH INOTIFY_RM_WATCH (FD OK, keine Events)

---

## Offen

### P10 — Code-Signing (Vertrauenskette)

Nicht angefangen. Ed25519 Owner-Key, .cosmo_sig ELF-Section.
Pruefung bei execve/dlopen/kexec.

### P14 — Virtual Terminals (PTY + VT)

Nicht angefangen. Braucht: PTY, VT-Buffer, Font-Renderer,
ANSI-Emulation, VT-Switch (Ctrl+Alt+F1-F4).
Kein Framebuffer-earlycon — Bildschirm bleibt schwarz bis VT steht.
Boot-Messages nur auf COM1 + /proc/dmesg. Silent Boot.
Abhaengig von: virtio-input oder hv_kbd fuer Keyboard-Events.

### P16 — Power Management + ACPI

Microkernel-Split: Kernel stellt Device-Nodes bereit,
Userspace-Daemons machen den Rest. Keine neuen Syscalls.

Kernel (Device-Nodes, ~200 Zeilen):
- [ ] /dev/msr — lseek(fd, MSR_NR, SEEK_SET) + read/write(fd, &val, 8)
      MSR-Allowlist (kein IA32_LSTAR etc.), is_driver bei open
- [ ] /dev/port — lseek(fd, PORT, SEEK_SET) + read/write(fd, &val, width)
      I/O-Port Zugriff (ACPI PM-Register, EC), is_driver bei open

Zugriff ueber bestehende Syscalls: open + lseek + read + write.
Wie Linux /dev/cpu/N/msr und /dev/port.

Userspace-Daemons (ueber svcmgr):
- [ ] powerd — HWP P-States (MSR IA32_HWP_REQUEST), CPU-Temp
      (MSR IA32_THERM_STATUS), Thermal-Throttle Policy.
      RT-Cores: max perf. Idle POSIX-Cores: power-save.
- [ ] acpid — ACPI-Tabellen parsen (RSDP→XSDT→FADT),
      AML-Interpreter (minimal oder ACPICA portiert),
      Events (Lid, Power-Button) via GPE.
- [ ] batteryd — EC-Abfrage via /dev/port, _BST/_BIF,
      Status publishen (/proc oder IPC).
- [ ] backlightd — ACPI _BCL/_BCM oder GPU-Register.
- [ ] Shutdown/Reboot — PM1a_CNT via /dev/port write.

Reihenfolge:
  1. /dev/msr + /dev/port Device-Nodes (Kernel)
  2. powerd: HWP + Temperatur (kein ACPI noetig)
  3. Shutdown/Reboot via ACPI PM-Register
  4. ACPI-Tabellen parsen (statisch, kein AML)
  5. AML-Interpreter (Batterie, Backlight, Sleep)

---

## Naechste Schritte (Prioritaet)

```
Prio 1 — Interaktives CosmoOS:
  P14 Virtual Terminals + PTY   → Silent Boot, dann Bash auf Framebuffer
  dlopen/dlsym                  → Runtime Library Loading

Prio 2 — Notebook-tauglich:
  P16 /dev/msr + /dev/port      → Device-Nodes, keine neuen Syscalls
  P16 powerd                    → HWP + Temperatur (Userspace)
  P16 Shutdown/Reboot           → ACPI PM-Register (Userspace)
  P10 Code-Signing              → Vertrauenskette (Ed25519)

Prio 3 — Node.js / Claude Code:
  INOTIFY Events                → Echte File-Watching Events liefern
  dlopen/dlsym                  → Runtime Library Loading

Prio 4 — Vollstaendigkeit:
  virtio-console/snd/fs         → Volle QEMU-Unterstuetzung
  IPv6 SLAAC                    → Dual-Stack Networking
  State-Transfer (P8)           → Graceful Driver Restart
  P16 ACPI AML                  → Batterie, Backlight, Sleep
```
