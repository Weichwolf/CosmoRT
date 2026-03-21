# CosmoRT — Offene Punkte

Stand: 2026-03-21.

ktest: 51 PASS, 0 FAIL, 0 SKIP.
94 Syscalls implementiert. CosmoFS persistent auf virtio-blk.

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

### P6 — Signal User-Handler + sigreturn
- [x] k_sigaction struct (handler, flags, restorer, mask) in process_t
- [x] deliver_signal(): Signal-Frame auf User-Stack (ucontext + siginfo)
- [x] SYS_RT_SIGRETURN (15): Register aus Signal-Frame wiederherstellen
- [x] SA_RESTORER Support (libc-kompatibler Trampoline)
- [x] On-stack Trampoline Fallback (mov rax,15; syscall)
- [x] Signal-Delivery in SYSCALL-Path, INT 0x80-Path, Timer-Preempt-Path
- [x] Signal-Blocking waehrend Handler (sa_mask + auto-block)
- [x] sigprocmask-basiertes Blocking/Unblocking mit Delivery bei Unblock
- [x] Fork erbt sig_actions + sig_blocked
- [x] 13 Signal-Tests in ktest (SIG_IGN, User-Handler, sigreturn, blocking)

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

## P11 — Netzwerk Zero-Config

Personal OS: Netzwerk muss einfach funktionieren. Keine Konfiguration.
Kabel rein oder WLAN verbinden → Netz laeuft.

### Automatisch bei jedem Boot

```
IPv4:       DHCP Client (~300 Zeilen)
IPv6:       SLAAC Stateless Autoconfiguration (~200 Zeilen)
Link-Local: 169.254.x.x / fe80:: (immer, auch ohne Router)
DNS:        DHCP Option 6 (automatisch)
Hostname:   mDNS .local — cosmo-XXXX.local
            XXXX = letzte 4 Hex der MAC-Adresse
            MAC 52:54:00:12:34:56 → cosmo-3456.local
            Eindeutig im LAN, auch bei mehreren CosmoOS-Rechnern
```

### Kein

- /etc/network/interfaces
- NetworkManager / systemd-networkd
- ip addr add / ifconfig
- Manuelle DNS-Konfiguration

Einzige User-Interaktion: WLAN-Passwort eingeben. Alles andere
ist Maschinenarbeit.

---

## P12 — Virtio Transport + Treiber (QEMU/KVM)

Gemeinsamer virtio-pci Transport-Layer (aus virtio-blk extrahieren),
dann alle Geraete als duenne Adapter.

### Gemeinsamer Transport (~300 Zeilen, einmal)

```c
// src/drivers/virtio/virtio.c
int  virtio_pci_init(virtio_dev_t *dev, uint16_t device_id);
int  virtqueue_add(virtqueue_t *vq, void *bufs[], int nbufs, int nout);
void virtqueue_kick(virtqueue_t *vq);
void *virtqueue_get(virtqueue_t *vq, uint32_t *len);
void virtio_set_features(virtio_dev_t *dev, uint64_t features);
```

### Treiber

```
Haben:
  [x] virtio-blk     (306 Zeilen)

Fehlen:
  [ ] virtio-net      (~400 Zeilen) — Alternative zu E1000
  [ ] virtio-gpu      (~600 Zeilen) — 2D Framebuffer
  [ ] virtio-input    (~300 Zeilen) — Tastatur + Maus
  [ ] virtio-console  (~200 Zeilen) — Terminal
  [ ] virtio-snd      (~400 Zeilen) — Audio (RT)
  [ ] virtio-fs       (~500 Zeilen) — Host-FS Sharing (Dev-Workflow)
```

---

## P13 — Hyper-V Support (Primaere Entwicklungsplattform)

CosmoRT soll direkt in Hyper-V/WSL2 laufen. Hyper-V nutzt nicht
virtio sondern VMBus (Ring-Buffer + Hypercalls). Komplett eigenes
Transport-Protokoll.

### Hyper-V Enlightenments (Kernel-Core)

Im Kernel selbst, nicht in Treibern. Erkennung via CPUID Leaf 0x40000001.

```
  [ ] Hyper-V Detection     — CPUID 0x40000001 "Microsoft Hv"
  [ ] Synthetic MSRs        — HV_X64_MSR_GUEST_OS_ID etc.
  [ ] SynIC                 — Synthetic Interrupt Controller
                               (ersetzt APIC fuer VM-Interrupts)
  [ ] Reference TSC Page    — Zeitmessung ohne VMEXIT
                               (shared page, rdtsc + scale/offset)
  [ ] Hypercall Page        — HvPostMessage, HvSignalEvent
  [ ] Crash MSRs            — Crash-Info an Host melden
  [ ] PV Spinlocks          — HvCallNotifyLongSpinWait
                               (verhindert Spinlock-Starvation in VM)
```

~500 Zeilen im Kernel-Core (src/kernel/hyperv.c).

### VMBus Transport-Layer (~800 Zeilen)

```
src/drivers/hyperv/vmbus.c

Ring-Buffer ueber Shared Memory:
  Host allociert Speicher
  → Guest mappt via GPADL (Guest Physical Address Descriptor List)
  → Ring-Buffer: Upstream (Guest→Host) + Downstream (Host→Guest)
  → Signalisierung via SynIC (kein PCI, kein MMIO)

Initialisierung:
  1. Hypercall HvPostMessage → VMBus INITIATE_CONTACT
  2. Host antwortet mit VERSION_RESPONSE
  3. Channel Offer (ein Channel pro Geraet)
  4. Guest oeffnet Channel → Ring-Buffer Setup
  5. I/O via Ring-Buffer + HvSignalEvent
```

### Hyper-V Synthetic Treiber

```
  [ ] storvsc    (~500 Zeilen) — Synthetischer SCSI/Block
                                  Ersetzt virtio-blk
  [ ] netvsc     (~500 Zeilen) — Synthetischer Netzwerk-Adapter
                                  RNDIS-Protokoll ueber VMBus
  [ ] hyperv_fb  (~400 Zeilen) — Synthetischer Framebuffer
                                  Synthvid-Protokoll ueber VMBus
  [ ] hv_kbd     (~200 Zeilen) — Synthetische Tastatur
  [ ] hv_mouse   (~200 Zeilen) — Synthetische Maus
  [ ] hv_utils   (~300 Zeilen) — Heartbeat, Time Sync, Shutdown
                                  Antwort auf Host-Requests
```

### Reihenfolge (schnellster Weg zu laufendem Hyper-V)

```
13.1 Hyper-V Detection + Enlightenments (CPUID, MSRs)     ~200 Zeilen
13.2 SynIC Setup (Synthetic Interrupts)                    ~200 Zeilen
13.3 Hypercall Page + HvPostMessage                        ~100 Zeilen
13.4 VMBus Init + Channel Open                             ~500 Zeilen
13.5 storvsc (Block-Device → CosmoFS)                      ~500 Zeilen
     → CosmoRT bootet in Hyper-V mit Disk
13.6 netvsc + RNDIS (Netzwerk)                             ~500 Zeilen
     → Netzwerk funktioniert
13.7 hyperv_fb (Display)                                   ~400 Zeilen
13.8 hv_kbd + hv_mouse (Input)                             ~400 Zeilen
     → Interaktives CosmoOS in Hyper-V
13.9 hv_utils (Heartbeat, Shutdown)                        ~300 Zeilen
     → Sauberes Herunterfahren via Hyper-V Manager
```

Gesamt: ~3100 Zeilen. Davon sind 13.1-13.5 (~1500 Zeilen) der
minimale Pfad zum bootfaehigen CosmoRT in Hyper-V.

### Abstraktion

Treiber registrieren sich beim gleichen Kernel-Interface:
- storvsc → blk_register() (wie virtio-blk)
- netvsc → net_nic_register() (wie E1000)
- hyperv_fb → fb_register() (neu)

Kernel-Code aendert sich nicht. Nur der Treiber ist anders.
Boot-Erkennung: CPUID → Hyper-V? → VMBus-Treiber laden.
Kein CPUID Match? → PCI Scan → virtio-Treiber laden.

---

## Naechste Schritte (Prioritaet)

```
Prio 1 (jetzt):
  P13 Hyper-V Support         → Primaere Entwicklungsplattform

Prio 2 (wenn Hyper-V laeuft):
  P11 Netzwerk Zero-Config    → DHCP/SLAAC/mDNS, cosmo-XXXX.local
  P12 Virtio-Treiber          → Volle QEMU/KVM-Unterstuetzung
  P7  Dynamischer Linker      → Hot-Reload fuer Claude Code
  P8  Userspace-Treiber       → Crash-Isolation, Hot-Reload

Prio 3 (wenn CosmoOS produktiv ist):
  P9  kexec                   → Kernel-Updates ohne Reboot
  P10 Code-Signing            → Vertrauenskette (Ed25519, Owner-Key)
```

Schnellster Weg zu "CosmoOS in Hyper-V mit Claude Code":
P6 (Signale) → P13.1-13.5 (Boot) → P13.6 (Netz) → Node.js → Claude Code

---

## P14 — Virtual Terminals (Ctrl+Alt+F1-F4 Desktop-Switching)

4 virtuelle Terminals. Ctrl+Alt+F1-F4 switcht welches sichtbar ist.
Jede VT hat eigene Bash-Instanz. Dem Kernel ist egal ob auf
einer VT eine Shell oder ein Window-Manager laeuft.

```
F1: bash --login    (tty1, Shell)
F2: bash --login    (tty2, Shell)
F3: cosmo-wm        (tty3, Window-Manager — spaeter)
F4: bash --login    (tty4, Shell)
```

### Komponenten

1. **VT-Buffer** (4 × Zeichenbuffer + Attribut)
   Pro VT: 80×25 (oder dynamisch nach FB-Aufloesung)
   Buffer enthaelt Zeichen + Farbe/Attribute
   Aktive VT wird in den Framebuffer gerendert

2. **PTY (Pseudo-Terminal)** (~300 Zeilen)
   Master/Slave-Paar pro VT
   Bash schreibt in Slave → Master liest, rendert in VT-Buffer
   Tastatur-Input → Master schreibt in Slave → Bash liest

3. **VT-Switch** (~100 Zeilen)
   Keyboard-Handler: Ctrl+Alt+F1-F4 → active_vt = n
   Framebuffer neu zeichnen aus dem Buffer der aktiven VT
   Hintergrund-VTs laufen weiter (Bash-Prozesse stoppen nicht)

4. **Font-Renderer** (~200 Zeilen)
   Bitmap-Font (8x16 eingebettet, wie VGA)
   Glyph → Pixel im Framebuffer

5. **VT-Emulation** (~400 Zeilen)
   ANSI Escape-Sequenzen: Cursor-Move, Farbe, Clear, Scroll
   Genug fuer bash prompt, vim, top

### Kein Window-Manager noetig

VTs sind generisch. Ob Text oder Grafik ist dem Kernel egal.
Ein Window-Manager ist einfach ein Programm das:
- Den Framebuffer seiner VT per mmap bekommt
- Maus/Tastatur-Events liest
- Fenster rendert statt Text-Glyphen

Gleiche VT-Infrastruktur, anderer Client.

### Abhaengigkeiten

Braucht:
- Framebuffer-Zugriff (virtio-gpu oder hyperv_fb)
- Keyboard-Input (virtio-input oder hv_kbd)
- PTY-Syscalls: SYS_OPENAT fuer /dev/ptmx, SYS_IOCTL fuer TIOCGWINSZ

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
