# CosmoRT — Offene Punkte

Stand: 2026-03-21.

ktest: 38 PASS, 0 FAIL, 0 SKIP.
Kernel: 13664 Zeilen (9871 C + 1718 ASM + 2075 Headers).
93 Syscalls implementiert. CosmoFS persistent auf virtio-blk.

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
- [x] ChaCha20 CSPRNG (random.c — 6 Entropie-Quellen)
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

### P5 — CosmoFS + virtio-blk (Persistentes Dateisystem)
- [x] virtio-blk Treiber (306 Zeilen)
- [x] Block-Cache LRU (bcache.c, 222 Zeilen)
- [x] B+ Tree generisch (btree.c, 738 Zeilen)
- [x] Journal WAL (journal.c, 276 Zeilen)
- [x] CosmoFS Core (cosmofs.c, 565 Zeilen)
- [x] VFS Mount-Layer (cosmofs in vfs.c integriert)
- [x] mkfs.cosmo (tools/mkfs.c, 181 Zeilen)
- [x] disk.img 64MB fuer QEMU

---

## P6 — Signal User-Handler + sigreturn

Signal-Delivery an Userspace fehlt: check_pending_signals() macht
SIG_DFL (kill) und SIG_IGN, aber KEINE User-Handler.

Fuer Ruby/Node.js auf CosmoRT brauchen wir:
- Signal-Frame auf User-Stack pushen (Register + siginfo_t)
- sigreturn Syscall (SYS_RT_SIGRETURN 15) zum Zurueckkehren
- SA_SIGINFO Support (siginfo_t + ucontext_t an Handler)
- SA_RESTART fuer syscall restart nach Signal

Ohne das: kein GC (Ruby), kein V8 (Node.js), kein Claude Code.

### Implementierung (~500 Zeilen)

```
1. Signal-Frame Layout (auf User-Stack):
   [padding fuer Alignment]
   [ucontext_t: uc_mcontext mit allen Registern]
   [siginfo_t: si_signo, si_errno, si_code, ...]
   [sa_restorer Trampoline-Adresse (→ SYS_RT_SIGRETURN)]

2. deliver_signal() in sched_preempt/syscall_return:
   - User-Stack-Pointer senken
   - Signal-Frame schreiben
   - RIP auf Handler setzen, RDI=signo, RSI=&siginfo, RDX=&ucontext
   - Return to userspace → Handler laeuft

3. SYS_RT_SIGRETURN:
   - Signal-Frame vom Stack lesen
   - Register wiederherstellen
   - Zurueck zum unterbrochenen Code
```

---

## P7 — Dynamischer Linker (ld-cosmo.so)

Fuer Hot-Reload von libc, Treibern, Services ohne Reboot.
Claude Code auf CosmoOS muss libc-Aenderungen live einspielen koennen.

### Was es braucht

- ELF-Loader erweitern: PT_INTERP erkennen → ld-cosmo.so laden
- ld-cosmo.so: Shared Libraries laden, Symbole aufloesen, Relocations
- dlopen/dlsym als echte Implementierung (nicht Stubs)
- Position-Independent Executables (PIE) als Default
- GOT/PLT Handling

### Warum

Ohne dynamischen Linker: jede libc-Aenderung erfordert Neukompilierung
ALLER Programme + Neustart. Mit dynamischem Linker: neue libc.so
austauschen, neue Prozesse nutzen sie automatisch.

Kritisch fuer: Claude Code entwickelt CosmoOS im laufenden Betrieb.

---

## P8 — Userspace-Treiber + Service-Manager

Treiber aus dem Kernel extrahieren. CosmoRT ist ein Microkernel —
Treiber SOLLTEN im Userspace laufen.

### Was es braucht

- Treiber-Prozesse die ueber IPC mit dem Kernel kommunizieren
- Service-Manager: start/stop/restart/health-check
- Graceful Driver Restart: State-Transfer bei Treiber-Update
- Hot-Reload: Claude Code aendert Treiber, neustartet ohne Reboot

### Reihenfolge

1. virtio-blk Treiber → Userspace extrahieren (Referenz)
2. E1000 NIC → Userspace extrahieren
3. Service-Manager (einfach: fork+exec+waitpid+restart)
4. State-Transfer-Protokoll (IPC-basiert)

---

## P9 — kexec (Kernel Hot-Swap)

Fuer Kernel-Aenderungen ohne vollen Reboot. ~2 Sekunden Downtime.

```c
// SYS_KEXEC: Neuen Kernel laden und ausfuehren
int kexec(const void *kernel, size_t len, const char *cmdline);
```

Implementierung:
- Neuen Kernel in hohen Speicher kopieren
- Alle Prozesse stoppen
- Hardware zuruecksetzen (APIC, Interrupts)
- Zum neuen Kernel springen
- Neuer Kernel bootet, init startet alles neu

---

## P10 — Code-Signing (Vertrauenskette)

Kein Live-Patching. Stattdessen: nur signierter Code darf ausgefuehrt
werden. Schuetzt gegen Manipulation, korrupte Builds, Malware.

### Vertrauenskette

```
UEFI Secure Boot
  → prueft Kernel-Signatur (Owner-Key)
    → Kernel prueft Treiber-Signaturen (Hot-Swap)
    → Kernel prueft Service-Signaturen (Restart)
    → kexec nur mit signiertem Kernel
    → dlopen nur signierte .so
```

### Owner-Key (KEIN zentraler Gatekeeper)

- Einmalig generiert, auf dem Geraet gespeichert
- Ed25519 (klein, schnell, sicher)
- DU signierst was auf DEINEM Rechner laeuft
- Kein App Store, kein Microsoft/Apple Trust-Modell
- BeOS-Philosophie: dein Computer, deine Regeln

### Workflow fuer Claude Code

```
Claude Code aendert Treiber
→ cc -o new_driver.so driver.c
→ cosmo_sign new_driver.so          (signiert mit Owner-Key)
→ service-manager restart driver    (prueft Signatur)
→ Hot-Swap passiert

Ohne gueltigen Key: kein kexec, kein Treiber-Load, kein dlopen.
Schuetzt auch gegen korrupte Builds (Hash stimmt nicht).
```

### Implementierung

- cosmo_sign Tool (Host + Target): Ed25519 Sign/Verify (~200 Zeilen)
- ELF-Section .cosmo_sig fuer Signatur (64 Bytes am Ende)
- Kernel prueft bei execve/dlopen/kexec
- Owner-Key in /etc/cosmo/owner.pub (oeffentlich)
- Private Key in /etc/cosmo/owner.key (nur root-lesbar, kein Export)

---

## Naechste Schritte (Prioritaet)

```
P6 Signal User-Handler     → Ruby/Node.js auf CosmoRT
P7 Dynamischer Linker      → Hot-Reload fuer Claude Code
P8 Userspace-Treiber       → Crash-Isolation, Hot-Reload
P9 kexec                   → Kernel-Updates ohne Reboot
P10 Code-Signing           → Vertrauenskette, kein unsignierter Code
```

Der kritische Pfad fuer "Claude Code entwickelt CosmoOS":
P6 (Signale) → Claude Code laeuft → P7 (dyn. Linker) → P10 (Signing) → sichere Live-Entwicklung

---

## CosmoPX-Integration Checkliste

Syscalls die CosmoPX libc nutzt aber CosmoRT noch nicht hat:

```
Haben (93):  read write open close stat fstat mmap munmap mprotect
             brk clone fork execve wait4 kill pipe2 dup2 getcwd
             chdir mkdir rmdir unlink rename getdents64 ioctl fcntl
             readv writev poll socket connect bind listen accept
             sendto recvfrom sendmsg recvmsg getsockname getpeername
             setsockopt getsockopt shutdown socketpair futex
             clock_gettime clock_nanosleep nanosleep gettimeofday
             getpid getppid gettid getuid geteuid getgid getegid
             uname access getrandom arch_prctl rt_sigaction
             rt_sigprocmask sched_yield set_tid_address prlimit64
             set_robust_list rseq mlock mlockall munlock munlockall
             + 7 CosmoOS-spezifische (HW Primitives + NIC)

Fehlen fuer CosmoPX:
  SYS_FCHMOD (91)        — chmod
  SYS_FCHOWN (93)        — chown
  SYS_LINK (86)          — hard links
  SYS_SYMLINK (88)       — symbolic links
  SYS_READLINK (89)      — readlink
  SYS_TRUNCATE (76)      — truncate
  SYS_FTRUNCATE (77)     — ftruncate
  SYS_EPOLL_* (232-291)  — epoll (Node.js braucht das)
  SYS_EVENTFD (284)      — eventfd (libuv)
  SYS_SIGNALFD (282)     — signalfd
  SYS_TIMERFD_* (283-86) — timerfd (libuv)
  SYS_INOTIFY_* (253-55) — inotify (File-Watching, Node.js)
  SYS_DUP3 (292)         — dup3
  SYS_PIPE (22)          — pipe (alt, haben pipe2)
  SYS_LSTAT (6)          — lstat
  SYS_OPENAT (257)       — relativ open (haben wir, pruefen)
  SYS_MKNODAT (259)      — mknod
  SYS_FCHMODAT (268)     — fchmodat
  SYS_FSTATAT (262)      — fstatat
  SYS_UTIMENSAT (280)    — timestamps setzen
  SYS_FALLOCATE (285)    — pre-allocate
  SYS_SYSINFO (99)       — system info
  SYS_GETRUSAGE (98)     — resource usage
  SYS_SETRLIMIT (160)    — resource limits setzen
  SYS_TIMES (100)        — process times
```

Fuer "Hello World" reicht was wir haben.
Fuer Ruby: + Signale (P6) + einige fehlende Syscalls.
Fuer Node.js: + epoll + eventfd + timerfd + inotify.
Fuer Claude Code: alles oben.
