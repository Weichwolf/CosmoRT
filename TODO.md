# CosmoRT — Offene Punkte

Stand: 2026-03-21. ~25K LOC, 123 Syscalls.
ktest: 61 PASS, 0 FAIL. Kein Polling (IRQ-driven).

---

## Audit 2 — 2026-03-21

### SEC-CRIT (2)

- [ ] do_poll TOCTOU: schreibt direkt in User-pollfd[] ohne Kernel-Bounce
      socket.c:188-218. Concurrent Thread kann fds[i].fd zwischen Check und
      revents-Write mutieren → OOB fd-Zugriff.
      Fix: pollfd-Array auf Kernel-Stack kopieren (max 256), Ergebnis zurueckschreiben.

- [ ] do_sendto TOCTOU: User-Buffer geht direkt an NIC DMA
      socket.c:103-111. Zwischen user_ok und NIC-Copy kann User remappen.
      Fix: Kernel-Bounce-Buffer (max 1400 Bytes pro TCP-Segment).

### SEC-HIGH (4)

- [ ] do_ioctl TIOCGWINSZ: schreibt direkt an User-Pointer ohne Bounce
      syscall.c:2039-2047. Unmapped VMA → Kernel #PF.
      Fix: Kernel-Stack struct, dann kmemcpy.

- [ ] do_rt_sigaction: liest/schreibt User-Struct direkt
      syscall.c:1302-1305. Unmapped VMA → Kernel #PF.
      Fix: Kernel-Stack, kmemcpy rein/raus.

- [ ] inotify_add_watch: ino_strncpy statt copy_path_from_user
      epoll.c:639-640. String > 255 Bytes liest in Kernel-Adressraum.
      Fix: copy_path_from_user(kpath, path, 256).

- [ ] vfs_read/vfs_write: User-Buffer direkt an CosmoFS ohne Bounce
      vfs.c:536,602. Unmapped VMA → Kernel #PF, TOCTOU moeglich.
      Fix: 4KB-Chunks ueber Kernel-Buffer bouncen.

### SEC-MED (5)

- [ ] HW-Primitive Syscalls geben Kernel-Adressen an Userspace
      syscall.c:2404-2417. MMIO/DMA-Pointer sind Direct-Map, nicht User-mappbar.
      Fix: map_user_page statt Direct-Map Pointer (fuer echte Userspace-Treiber).

- [ ] copy_path_from_user: kein Page-Fault-Handling
      syscall.c:35-43. Valid-but-unmapped VMA → Kernel #PF.
      Fix: Kernel-Mode #PF Handler (siehe ARCH unten).

- [ ] Signal-Trampoline auf NX-Stack
      syscall.c:1119-1130. W^X Stack hat kein PROT_EXEC → #PF bei sigreturn.
      Fix: SA_RESTORER erzwingen oder Stack-VMA pruefen.

- [ ] DHCP XID = 0xDEADBEEF (statisch, vorhersagbar)
      net.c:341. Triviales DHCP-Spoofing.
      Fix: random_get() fuer XID.

- [ ] DNS txid aus timer_ms() (niedrige Entropie)
      net.c:625. DNS Cache Poisoning moeglich.
      Fix: random_get() fuer txid und Source-Port.

### CORR-HIGH (5)

- [ ] do_brk Shrink leakt Pages — nie freigegeben
      syscall.c:277-282. Langlebige Prozesse laufen in OOM.
      Fix: PTE-Walk und page_free fuer schrumpfenden Bereich.

- [ ] do_fork stoppt RUNNING Threads auf anderen Cores nicht
      process.c:557-562. Nur RUNNABLE wird suspended, RUNNING laeuft weiter
      waehrend copy_address_space → inkonsistente Kopie.
      Fix: IPI an Cores mit Parent-Threads, auf ACK warten, dann kopieren.

- [ ] vfs_file refcount nicht atomic
      vfs.c:158-168. f->refcount++ ohne Lock/Atomic. Paralleles fork/close → Race.
      Fix: __sync_fetch_and_add / __sync_sub_and_fetch.

- [ ] do_dup2/dup3: kein Refcount-Increment fuer non-file FDs
      syscall.c:1242,1263. Kopiert raw Pointer. Pipe-close dekrementiert doppelt.
      Fix: Refcount fuer alle FD-Typen bei dup.

- [ ] do_fork: FD_PIPE/SOCKET/EPOLL/EVENTFD/TIMERFD/INOTIFY ohne Refcount
      process.c:597-601. Nur FD_FILE bekommt refcount++. Close in Parent oder
      Child korrumpiert den anderen → Use-after-free.
      Fix: Generischer Refcount fuer alle FD-backed Objects.

### CORR-MED (6)

- [ ] proc_find O(n) Scan ohne Lock
      process.c:373-379. Concurrent proc_cleanup → stale Pointer.
      Fix: Lock oder CAS auf State.

- [ ] build_user_stack str_off Underflow
      process.c:733-745. Grosses argv/envp → Schreiben ausserhalb der Page.
      Fix: Bounds-Check vor jedem String-Copy.

- [ ] execve Stack-Rebuild: selbes str_off Underflow
      process.c:1033-1068.
      Fix: Bounds-Check.

- [ ] do_wait4 schreibt wstatus direkt an User-Pointer
      process.c:1141-1142. Unmapped VMA → Kernel #PF.
      Fix: Kernel-Variable, kmemcpy.

- [ ] random_add_interrupt_entropy: Race auf csprng_state
      random.c:185-186. XOR ohne rng_lock, concurrent mit random_get.
      Fix: rng_lock vor Zugriff auf csprng_state.

- [ ] pipe_slab_ensure: Race auf Init-Flag
      syscall.c:1501-1507. Zwei concurrent pipe2 → double-init.
      Fix: __sync_bool_compare_and_swap(&pipe_slab_inited, 0, 1).

- [ ] sig_pending/sig_blocked ohne Atomic Ops
      syscall.c:1155-1160,1143,1385. |= und &= nicht atomic auf 64-Bit.
      Fix: __atomic_fetch_or / __atomic_fetch_and.

### PERF-HIGH (2)

- [ ] proc_find O(n) bei jedem kill/wait
      process.c:373-379. PROC_MAX=16 OK, skaliert nicht.
      Fix: PID→Slot Direktindex.

- [ ] find_thread_by_tid O(n) Scan (THREAD_MAX=64)
      futex.c:69-77, ipc.c:17-26. Jeder PI-Futex und IPC-Wake.
      Fix: TID→Pool-Index Direktindex.

### PERF-MED (3)

- [ ] epoll_wait haelt Lock waehrend fd_poll_readiness Scan
      epoll.c:240-258. IRQs disabled ueber gesamten FD-Scan → RT-Latenz.
      Fix: Entries unter Lock kopieren, Lock loslassen, dann scannen.

- [ ] do_munmap: O(n) vma_find pro Page (262144 Lookups fuer 1GB)
      syscall.c:557-581.
      Fix: VMAs in Adressreihenfolge walken statt Page-by-Page proben.

- [ ] do_mmap MAP_FIXED: selbes O(n)-pro-Page Pattern
      syscall.c:414-438.
      Fix: VMA-Tree in Adressreihenfolge walken.

---

## Architektur

### Kernel-Mode #PF Handler (PRIO 1)

Fehlt komplett. Jeder Kernel-Zugriff auf valid-but-unmapped User-Memory
(demand paging) fuehrt zu Triple-Fault. Betrifft: copy_path_from_user,
do_rt_sigaction, do_wait4, do_ioctl, jede Stelle die User-Pointer direkt
dereferenziert.

Fix: #PF Handler der bei Kernel-Mode Faults auf User-Adressen die VMA
prueft, Page demand-mapped, und zurueckkehrt. Foundation fuer alles andere.

### FD Refcounting (PRIO 2)

Nur vfs_file hat Refcount. Pipe, Socket, Epoll, Eventfd, Timerfd, Inotify
haben keinen. Fork und dup2/dup3 kopieren raw Pointer → Use-after-free
bei Close. Root Cause fuer 2 CORR-HIGH Findings.

Fix: Generischer Refcount in fd_entry_t oder pro Objekt-Typ.

### Netzwerk Per-Queue Lock

net.c:69. Ein globaler Lock fuer 5 Packet-Queues (TCP, UDP-DHCP,
UDP-DNS, ARP, ICMP). TCP blockiert ARP und umgekehrt.

Fix: Per-Queue Spinlock.

---

## Erledigt

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
- [ ] dlopen/dlsym/dlclose (Runtime-Loading)

### P8 — Userspace-Treiber
- [x] e1000d, svcmgr, net_port Ring-IPC, SYS_COSMO_NIC_ATTACH
- [ ] State-Transfer-Protokoll fuer Graceful Restart

### P9 — kexec
- [x] ELF-Validation, bcache/journal Flush, AP Stop, Identity-Trampoline, SYS_COSMO_KEXEC

### P10 — Code-Signing
- [ ] Ed25519 Owner-Key, .cosmo_sig ELF-Section, Pruefung bei execve/dlopen/kexec

### P11 — Netzwerk Zero-Config
- [x] DHCP Client, mDNS Responder
- [ ] IPv6 SLAAC
- [ ] Link-Local 169.254.x.x Fallback

### P12 — Virtio Transport
- [x] Gemeinsamer Transport, virtio-blk, virtio-net, virtio-gpu, virtio-input
- [ ] virtio-console, virtio-snd (Audio RT), virtio-fs (Host-FS)

### P13 — Hyper-V
- [x] Detection, Hypercall Page, Reference TSC, VMBus, storvsc, netvsc, hyperv_fb, hv_kbd, hv_mouse, hv_utils

### P14 — Virtual Terminals
- [x] PTY (pty.c), VT-Buffer + ANSI-Parser (vt.c), Font-Renderer (fb.c), Framebuffer
- [x] VT-Switch (Ctrl+Alt+F1-F4), Keyboard-Input (input.c)
- [ ] BUG: Scancode-Mapping — virtio-input liefert Linux KEY_* Codes,
      VT Keymap erwartet USB HID. Braucht Uebersetzungstabelle.
- [ ] BUG: Echo unsichtbar — pty_master_write schreibt Echo in output_buf,
      aber vt_flush nur bei Syscalls. Timer-Tick sollte vt_flush(active_vt) aufrufen.

### P15 — Syscalls fuer CosmoCL
- [x] DUP3, PIPE, SYSINFO, GETRUSAGE, PRLIMIT64, TIMES
- [x] FCHMOD, FCHOWN, SYMLINK, READLINK, TRUNCATE, FTRUNCATE
- [x] LSTAT, MKNODAT, FCHMODAT, FSTATAT, UTIMENSAT, FALLOCATE, Symlinks
- [x] EPOLL, EVENTFD, TIMERFD, SIGNALFD (Stub), INOTIFY (FD OK, keine Events)
- [ ] INOTIFY echte Events liefern

### P16 — Power Management + ACPI
- [ ] /dev/msr, /dev/port Device-Nodes
- [ ] powerd, acpid, batteryd, backlightd, Shutdown/Reboot (Userspace)

### procfs
- [x] /proc/dmesg, /proc/meminfo, /proc/cpuinfo

---

## Naechste Schritte (Prioritaet)

```
Prio 1 — Stabilitaet (Audit 2 Fixes):
  Kernel-Mode #PF Handler       → Foundation fuer User-Memory-Zugriffe
  FD Refcounting                 → Fork/dup Use-after-free fixen
  TOCTOU Kernel-Bounce           → do_poll, do_sendto, vfs_read/write
  Atomic Ops                     → sig_pending, vfs_file refcount

Prio 2 — Interaktives CosmoOS:
  VT Keyboard Fix                → Scancode-Mapping + Echo-Flush
  INOTIFY echte Events           → Node.js fs.watch
  dlopen/dlsym                   → Runtime Library Loading

Prio 3 — Notebook-tauglich:
  P16 /dev/msr + /dev/port       → Device-Nodes
  P16 powerd + Shutdown           → HWP, Temperatur, ACPI
  P10 Code-Signing                → Ed25519 Vertrauenskette

Prio 4 — Vollstaendigkeit:
  virtio-console/snd/fs           → Volle QEMU-Unterstuetzung
  IPv6 SLAAC                      → Dual-Stack Networking
  State-Transfer (P8)             → Graceful Driver Restart
  Per-Queue Net Lock               → TCP/ARP Entkopplung
```
