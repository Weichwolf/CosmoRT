# CosmoRT — Offene Punkte

Stand: 2026-03-21. ~28K LOC, 123 Syscalls.
ktest: 61 PASS, 0 FAIL. Kein Polling (IRQ-driven).
VT interaktiv (Bernstein-Palette, Hack Font, Tippen funktioniert).

---

## Audit 3 — 2026-03-22

Scope: Alle .c/.h in src/kernel/ (alle Subdirectories) und src/drivers/.
Post-Restrukturierung, Post-Audit-2-Fixes, neue Subsysteme (CosmoFS execve, Streaming ELF, Shared IRQ).

### SEC-CRIT (3/3 fixed)

- [x] SYS_COSMO_MMIO_MAP: Kernel schreibt direkt in User-Pointer ohne Bounce
      src/kernel/syscall/dispatch.c:279. `*(void **)a3 = virt;` — a3 ist User-Pointer,
      user_ok prueft nur 8 Bytes, aber der Kernel schreibt direkt dorthin statt ueber
      kmemcpy. SMAP ist nicht aktiv, aber wenn a3 auf eine Kernel-Adresse zeigt die
      user_ok durchlaesst (near 0x800000000000), ist das ein beliebiger Kernel-Write.
      Gleiches Problem bei SYS_COSMO_DMA_ALLOC (Zeile 287): `*(void **)a2 = virt;`.
      Fix: Bounce ueber lokale Variable + kmemcpy.

- [x] SYS_COSMO_PCI_READ: user_ok prueft a4 statt a5
      src/kernel/syscall/dispatch.c:299. `user_ok(a4, 4)` prueft das vierte Argument
      (reg), aber das Ergebnis wird nach `(uint32_t *)a5` geschrieben (Zeile 300).
      a5 ist der User-Pointer fuer den Output, wird nie validiert. Userspace kann
      beliebige Kernel-Adresse als a5 uebergeben → arbitrary kernel write eines
      PCI-Config-Werts.
      Fix: `user_ok(a5, 4)` statt `user_ok(a4, 4)`.

- [x] SYS_COSMO_IRQ_REGISTER: Userspace registriert beliebige Kernel-Funktion als IRQ-Handler
      src/kernel/syscall/dispatch.c:296. `cosmo_irq_register((int)a1, (void (*)(void *))a2, (void *)a3)`
      — a2 ist ein User-Pointer der als Funktionspointer interpretiert wird. IRQ-Handler
      laufen in Ring 0 im IRQ-Kontext. Ein Userspace-Treiber kann beliebige Kernel-Adressen
      als Handler eintragen → Code-Execution in Ring 0 mit beliebigem Kontext.
      Auch mit is_driver-Check: jeder Treiber-Prozess kann Kernel-Kontrolle uebernehmen.
      Fix: Handler-Adressen muessen im User-Segment liegen und ueber Trampoline aufgerufen
      werden, oder IRQ-Handler muessen als Kernel-Registrierung ueber IPC dispatched werden.

### SEC-HIGH (5)

- [ ] do_recvfrom: TCP-Daten direkt in User-Buffer ohne Bounce
      src/kernel/net/socket.c:125. `net_tcp_recv(&s->tcp, buf, ...)` schreibt direkt in den
      User-Buffer `buf`. net_tcp_recv kopiert Netzwerk-Pakete (aus NIC DMA) direkt dorthin.
      Keine Bounce-Buffer-Isolation. Ein racing Userspace-Thread kann den Buffer waehrend
      des Kopierens aendern (TOCTOU mit DMA).
      Fix: Bounce ueber Kernel-Buffer wie bei do_sendto.

- [ ] socket_write (via do_write FD_SOCKET): Kein Bounce-Buffer
      src/kernel/net/socket.c:141. `net_tcp_send(&s->tcp, buf, ...)` liest direkt aus
      User-Space buf. TOCTOU: Userspace kann Buffer nach Validierung aendern.
      do_sendto hat korrekt einen kbuf[1500]-Bounce, aber socket_write/socket_read nicht.
      Fix: Bounce wie in do_sendto.

- [ ] do_dup/do_dup2/do_dup3/F_DUPFD: FD-Kopie ohne Refcount-Inkrement
      src/kernel/syscall/dispatch.c:128 (dup), sys_file.c:328 (dup2), sys_file.c:349 (dup3).
      `p->fds.entries[newfd] = *old;` kopiert die fd_entry_t bitweise. Bei FD_FILE wird
      vfs_file_incref() nicht aufgerufen. Wenn der Original-FD geschlossen wird, sinkt
      refcount auf 0 und vfs_file wird freigegeben. Der duplizierte FD zeigt auf freed Memory.
      F_DUPFD in do_fcntl (Zeile 669) ruft incref nur fuer FD_FILE auf, aber dup/dup2/dup3 nicht.
      Fix: Refcount-Inkrement in do_dup, do_dup2, do_dup3 fuer FD_FILE (und andere Typen).

- [ ] do_rt_sigaction: oldact direkt in Userspace geschrieben ohne kmemcpy
      src/kernel/syscall/sys_signal.c:230. `*oldact = p->sig_actions[sig];` — direkte
      Zuweisung an User-Pointer. Leakt kompletten k_sigaction-Struct (inkl. sa_handler
      Kernel-Pointer falls SIG_DFL/SIG_IGN) direkt. Kein Problem weil 0/1, aber die
      Struct koennte sa_restorer enthalten (User-Pointer) und die Zuweisung umgeht
      jede Page-Fault-Sicherheit auf nicht-gemappte Adressen.
      Fix: kmemcpy(oldact, &p->sig_actions[sig], sizeof(...)).

- [ ] do_rt_sigprocmask: *oldset direkte Zuweisung an User-Pointer
      src/kernel/syscall/sys_signal.c:251. `*oldset = p->sig_blocked;` — selbes Problem.
      Fix: `uint64_t tmp = p->sig_blocked; kmemcpy(oldset, &tmp, 8);`

### SEC-MED (4)

- [ ] SYS_COSMO_FW_LOAD: Output-Pointer-Validierung aber kein Bounce
      src/kernel/syscall/dispatch.c:308. user_ok prueft a2/a3, aber cosmo_fw_load
      schreibt direkt `*(void **)a2` und `*(size_t *)a3`. Aktuell return -2 (stub),
      aber wenn implementiert: same arbitrary-write Pattern wie MMIO_MAP.
      Fix: Bounce-Buffer bei Implementierung.

- [ ] SYS_COSMO_NIC_ATTACH: 22-Byte user_ok, aber Struct hat Padding
      src/kernel/syscall/dispatch.c:312-315. Die Struct ist {uint64_t, uint64_t, uint8_t[6]}
      = 22 Bytes ohne Padding. Aber bei 64-bit Alignment koennte der Compiler die Struct
      auf 24 Bytes padden. sizeof(kargs) waere dann 24, kmemcpy liest 24 Bytes aus
      User-Space. user_ok prueft nur 22.
      Fix: user_ok(a1, sizeof(kargs)) statt hardcoded 22.

- [ ] PID/TID Wraparound: next_pid/next_tid inkrementieren ohne Wrap-Check
      src/kernel/proc/process.c:69,91. next_tid++ und next_pid++ wachsen unbegrenzt.
      PID_TABLE_MAX=256, TID_TABLE_MAX=512. Bei pid >= 256 wird pid_table[pid] nicht
      gesetzt (if-Guard), proc_find kann den Prozess nicht mehr finden → wait4/kill
      funktionieren nicht.
      Fix: next_pid wrappen und freie Slots suchen.

- [ ] pipe_slab_ensure: Race bei konkurrenter Initialisierung
      src/kernel/syscall/sys_ipc.c:22-27. `pipe_slab_inited` wird ohne Lock geprueft/gesetzt.
      Zwei gleichzeitige pipe2-Aufrufe auf verschiedenen Cores koennten slab_init doppelt
      aufrufen und die Free-List korrumpieren.
      Fix: Atomic compare-and-swap oder unter globaler Lock initialisieren.

### CORR-HIGH (5/5 fixed)

- [x] do_mmap MAP_FIXED: VMA-Splitting fehlerhaft bei innerem Split
      src/kernel/syscall/sys_mem.c:164-167. Wenn ein VMA den gesamten Bereich ueberlappt
      (ov->start < vaddr && ov->end > vaddr + length), wird ov->end auf vaddr gesetzt
      und ein neues VMA ab vaddr+length eingefuegt. Das neue VMA bekommt ov->prot/ov->flags
      — aber ov->prot wird NACH dem Split gelesen. Wenn der AVL-Tree den Knoten rotiert
      hat, koennte ov auf einen anderen Knoten zeigen. Zusaetzlich: die Schleife danach
      findet per vma_find(vaddr) erneut — findet jetzt das abgeschnittene VMA und bricht ab.
      Korrekt, aber fragil.
      Fix: Prot/Flags vor der Mutation lesen.

- [x] do_fork: fd_obj_incref fuer Pipes/Sockets erhoet keinen Refcount
      src/kernel/proc/process.c:642. `fd_obj_incref(ftype, parent->fds.entries[i].obj)`
      wird aufgerufen, aber die Funktion muesste tatsaechlich die Lebensdauer verlaengern.
      Fuer Pipes: die pipe_pool-basierte Implementierung hat keinen Refcount.
      Fuer Sockets: socket_t hat keinen Refcount. Wenn Parent und Child denselben
      Socket schliessen, wird der Socket doppelt in SOCK_UNUSED gesetzt und TCP
      doppelt geschlossen → Use-after-close.
      Fix: Refcounting fuer Pipe und Socket einfuehren.

- [x] do_exit: Prozess-Cleanup fehlt komplett
      src/kernel/syscall/sys_proc.c:35-73. do_exit setzt PROC_ZOMBIE und THREAD_DEAD,
      aber ruft NICHT proc_cleanup auf. FDs werden nie geschlossen, Sockets nie geschlossen,
      Pages nie freigegeben, VMAs nie freigegeben, Threads nie freigegeben. Jeder Prozess
      der exit() aufruft leakt seinen gesamten Adressraum.
      Fix: proc_cleanup(p) in do_exit aufrufen (nach Wake-Parent aber vor thread_return_to_kernel).

- [x] do_wait4: fehlende Implementierung oder Race
      Wenn do_wait4 existiert (process.c), muss es proc_cleanup aufrufen fuer ZOMBIE-Kinder.
      Wenn do_exit kein cleanup macht und do_wait4 es auch nicht macht, leaken alle
      Prozess-Ressourcen permanent.
      Fix: In do_wait4 proc_cleanup(child) nach Statusabfrage aufrufen.

- [x] vfs_close: Nicht-atomares Refcount-Decrement
      src/kernel/fs/vfs.c:500. `if (--f->refcount <= 0) file_free(f);` — pre-decrement
      ist nicht atomar. Zwei Threads die denselben FD (via dup) gleichzeitig schliessen,
      koennten beide refcount von 1 auf 0 dekrementieren und file_free doppelt aufrufen.
      Fix: __sync_sub_and_fetch (wie in vfs_file_free_obj, Zeile 178).

### CORR-MED (6)

- [ ] do_brk Shrink: Pages werden nicht freigegeben
      src/kernel/syscall/sys_mem.c:19-23. Kommentar sagt "page leak is acceptable",
      aber bei wiederholtem brk-Grow-Shrink-Zyklus leaken unbegrenzt Pages.
      Fix: Page-Table-Walk und page_free implementieren.

- [ ] timer_sleep_ms: Busywait < 10ms blockiert gesamten Core
      src/kernel/core/timer.c:76-78. Fuer ms < 10 wird RDTSC-Busywait verwendet.
      Korrekt fuer Hardware-Timing, aber ein Userspace-Prozess kann beliebig oft
      nanosleep(1ms) aufrufen und damit einen Core zu 100% belegen.
      Fix: Minimum auf Thread-Yield plus HLT.

- [ ] do_mlock: Infinite Loop bei VMA-Luecke
      src/kernel/syscall/sys_mem.c:102-109. Wenn vma_find NULL zurueckgibt, wird
      `va += 4096` gemacht. Bei grossen Ranges (z.B. 4GB) iteriert das ueber
      1M Seiten mit jeweils einem AVL-Lookup → extrem langsam.
      Fix: Zum naechsten VMA springen statt +4096.

- [ ] copy_path_from_user: Kein Page-Fault-Recovery
      src/kernel/syscall/dispatch.c:9-14. Byte-weise Kopie von User-Pointer. Wenn
      die Seite nicht gemappt ist, kommt ein Kernel-Page-Fault. Der Page-Fault-Handler
      (irq.c:169) mapped die Seite per Demand-Paging — funktioniert. Aber wenn der User
      absichtlich eine Adresse ohne VMA angibt, wird der Kernel-Panic-Pfad (Zeile 195)
      erreicht. Der Guard `cr2 < 0x800000000000ULL` hilft, aber kein VMA → kein Mapping
      → kein Resume → `KERNEL PANIC`. Userspace kann den Kernel crashen mit unmapped addr.
      Fix: Fehlerbehandlung im Page-Fault-Handler fuer Kernel-Mode-Faults ohne VMA
      (return -EFAULT statt Panic).

- [ ] do_readv: iovcnt 65..1024 wird auf 64 geclamped statt EINVAL
      src/kernel/syscall/sys_file.c:220-223. EINVAL fuer iovcnt > 1024, aber iovcnt 65-1024
      wird stillschweigend auf 64 reduziert. Linux gibt EINVAL fuer > IOV_MAX (1024).
      CosmoRT kopiert mehr iov-Entries als es verarbeitet, oder verarbeitet weniger als
      erwartet → Datenverlust bei grossen readv-Calls.
      Fix: EINVAL fuer iovcnt > 64 (der Stack-Buffer-Groesse).

- [ ] do_kill pid=-1: Signaliert nur eigenen Prozess statt alle
      src/kernel/syscall/sys_signal.c:278-280. POSIX: kill(-1, sig) sendet an alle
      Prozesse. CosmoRT sendet nur an self. Nicht critical, aber Compliance-Bug.
      Fix: Iteration ueber proc_pool.

### PERF-HIGH (2)

- [ ] do_poll: Busywait mit sti;hlt-Loop fuer Socket-FDs
      src/kernel/net/socket.c:199-228. Jede Iteration prueft alle FDs, macht net_poll(),
      dann hlt. Bei timeout=-1 (infinite) und keiner Socket-Aktivitaet wacht es bei
      jedem Timer-IRQ auf (100Hz) und scannt alles erneut. Kein Event-getriebenes Wakeup.
      Fix: Wakeup-Mechanismus wenn Daten ankommen statt Polling.

- [ ] do_epoll_wait: Selbes Problem
      src/kernel/event/epoll.c:235-272. sti;hlt-Loop mit Full-Scan bei jedem Timer-IRQ.
      Fix: Wakeup bei FD-State-Change.

### PERF-MED (3)

- [ ] do_munmap VMA-Probe: Page-weise Suche statt Range-Query
      src/kernel/syscall/sys_mem.c:301-304. `for (probe = start; probe < end; probe += 4096)`
      mit vma_find pro Seite. O(n_pages * log(n_vmas)) statt O(log(n_vmas)).
      Fix: vma_find_overlap verwenden (existiert bereits in vma.c:95).

- [ ] do_mmap MAP_FIXED: Selbes Problem beim Overlap-Scan
      src/kernel/syscall/sys_mem.c:157-158. Page-weise Probe mit vma_find.
      Fix: vma_find_overlap verwenden.

- [ ] inotify_event: Scannt alle Pool-Entries bei jedem VFS-Event
      src/kernel/event/epoll.c:527-550. INOTIFY_POOL_MAX=8 ist klein, aber jedes
      create/write/delete im VFS triggert einen O(pool * watches)-Scan unter Spinlock.
      Fix: Path-basierter Index oder Lazy-Evaluation.

---

## Naechste Schritte (Prioritaet)

```
Prio 0 — Aufraemen: ERLEDIGT
Prio 1 — Audit 3 SEC-CRIT + CORR-HIGH: ERLEDIGT (8/8)

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
