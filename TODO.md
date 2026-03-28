# CosmoRT — Offene Punkte

Stand: 2026-03-27. 1056+ ktest PASS (923 unit + 133 crash). Alpine Linux interaktive Shell funktioniert.

---

## Erledigt (Zusammenfassung)

Net Phase 0-E: TCP/UDP/ARP/IP/DNS/DHCP extrahiert, CUBIC (RFC 8312), SACK (RFC 2018),
Window Scaling (RFC 7323), OOO Buffering, Fast Retransmit/Recovery, Keepalive,
EINPROGRESS, SO_RCVTIMEO, Sleep/Wake statt Polling, TX-Ring.

RT/Compute A-E: SPSC Channels, IPI+Wake, TX-Ring, Timer-Wheel, Priorisierte Polling-Schleife.



MM: COW fork, MADV_FREE, Transparent Huge Pages (2MB).

Skalierung A-F: Socket Slab, TCP Hash Chaining, UDP Hash+Pool, ARP Hash+Pool,
Timer Free-Stack, FD Bitmap. Alles O(1).

SH-B: Device Nodes (/dev/null, /dev/zero, /dev/urandom, /dev/tty).

Security: 5 Fuzz-Fixes (fault_recover, sigreturn MXCSR, signal handler validation,
range checks, mlock DoS). Sec-B SMEP+SMAP (CR4 Bits, STAC/CLAC im Syscall-Entry/Exit
+ Signal-Frame), Sec-C W^X (mmap/mprotect RWX → -EINVAL), Sec-D Stack Guard Pages
(PROT_NONE VMA unter jedem Stack — war bereits implementiert).

Restrukturierung: include/linux/, include/kernel/ (Subsystem-Spiegel), src/kernel/sys/,
arch/x86_64/, drivers nach Bus sortiert, event split, arch.h Abstraktion.
Phase 1 File-Split: vfs.c→6, process.c→5, sys_signal.c→3 (4495→14 Dateien + 2 interne Header).
Test-Reorg: test/unit/ in 10 Unterordner (mm/fs/ipc/sched/signal/sys/hw/proc/net/perf).

SH-C: MAP_SHARED komplett (Page Cache, Demand Paging, Dirty Tracking + Write-Back).

Security: cosmo_dispatch EPERM fuer Non-Driver, is_driver nicht vererbt bei fork.

ELF-Loader: 4→1 Variante, CosmoFS→ext2 migriert (Layering-Fix), crt0.S (ABI),
AT_PHDR Fix, build_user_stack fuer proc_create_elf (argv/envp/auxv korrekt).

procfs: /proc/version, uptime, loadavg, filesystems, pid/status, pid/cwd, pid/environ,
bus/pci/devices, sys/kernel/pid_max, sys/kernel/hostname. /proc als Verzeichnis stat-bar.

Aufgeraeumt: CosmoPX→musl, e1000d/svcmgr/ktest.c/ld-cosmo.c/kbench.c/claude_init.c/vt_shell.c
geloescht (-3400 Zeilen). init.c minimal (15 Zeilen, execve /bin/sh).

Parallele Tests: fork pro Test (BATCH=4), MAP_SHARED Slots, Kernel-Fix orphaned Threads.

Alpine Bootstrap Phase 1-3: Interaktive Alpine Shell auf CosmoRT.
Dynamic Linking (ld-musl), ext2 Root, PTY/termios (TCGETS/TCSETS real,
poll()-Wake, timer-vt_flush, /dev/console, pty_input_direct).
ext2 Symlink-Resolution. make qemu-alpine bootet Alpine von Disk.

---

## Blocking eliminieren — Event-Queue fuer alle Syscalls

Problem: 15+ Stellen im Kernel setzen THREAD_BLOCKED + thread_return_to_kernel
direkt. Jede Stelle ist ein potentieller Hang-Bug (Wake verloren, Race, State-Fehler).
Job Control musste reverted werden weil wait4 nach fork+setpgid haengt.

### EQ-A: event_queue_t Infrastruktur — erledigt

event_queue_t pro Thread (16-Slot Ringbuffer). event_post (IRQ-safe, race-free),
event_wait (blocking mit Timeout), event_drain (WNOHANG). 35 Tests.

### EQ-B: Alle blocking Syscalls umgestellt — erledigt

15 Consumer + 12 Producer auf event_wait/event_post. sock_block_thread eliminiert.
THREAD_BLOCKED nur noch in event_wait. Netto -47 Zeilen.

### EQ-C: Job Control (auf Event-Queue Basis) — erledigt

THREAD_STOPPED State, TIOCSPGRP/TIOCGPGRP, Terminal Ctrl-C/Z/\ an fg_pgid,
SIGTSTP/SIGSTOP → THREAD_STOPPED, SIGCONT → resume, wait4 WUNTRACED/WCONTINUED
via process_t stop_signal/was_continued Flags (nicht event_drain — Race mit Queue
Corruption). sched_yield Signal-Check fuer Stop/Kill. 23 Tests.

## Offen — Self-Hosting Blocker

### SH-C: MAP_SHARED mmap

#### SH-C1: File-backed Shared Pages (Read-Only Coherency) — erledigt

Page Cache (1024 Buckets, Slab, Spinlock): (inode, offset) → physische Page.
MAP_SHARED file-backed nutzt Page Cache, fork teilt Pages, page_free evicted bei Refcount 0.
14 Tests.

#### SH-C2: VMA File-Backing + Demand Paging — erledigt

vma_t um file_ino, file_offset, file_backend erweitert. File-backed mmap lazy
(nur VMA anlegen, keine Pages). Page Fault Handler liest per vfs_pread_by_ino
bei Not-Present auf file-backed VMA (shared: Page Cache, private: eigene Kopie).
msync mit Signatur (addr, len, flags) + Validierung (Write-Back: SH-C3).
MS_ASYNC/MS_SYNC/MS_INVALIDATE in linux/mman.h. VMA-Splits propagieren File-Felder.
19 Tests (3 alt + 16 neue Checks).

#### SH-C3: Dirty Tracking + Write-Back — erledigt

Demand-paged shared file-backed Pages read-only gemappt. Write-Fault setzt
PTE_WRITE, CPU setzt PTE_DIRTY. msync(MS_SYNC) scannt PTEs, schreibt dirty
Pages per vfs_pwrite_by_ino zurueck, cleared PTE_DIRTY. munmap/exit: implizites
Write-Back vor Page-Freigabe. 19 Tests.

### SH-D: Signale — erledigt

SIGPIPE (Pipe+Socket+Unix), SIGALRM (alarm Syscall), SIGCHLD (Child-Exit),
SIGWINCH (TIOCSWINSZ). 13 Tests.

### SH-E: Terminal-Groesse — erledigt

TIOCGWINSZ/TIOCSWINSZ, winsize in PTY-State, PTY-Index Bug Fix. 9 Tests.

### SH-F: /proc erweitern — erledigt

/proc/self/exe, /proc/pid/cmdline, /proc/pid/stat, /proc/pid/maps,
/proc/meminfo, per-PID Routing. 15 Tests.

### SH-G: Locale + Timezone

- [ ] TZ Environment-Variable
- [ ] /etc/localtime: Timezone-Datei
- [ ] test: time() mit korrektem TZ Offset

### SH-H: Symlinks — erledigt

Symlinks komplett: ramfs + ext2, path resolution mit ELOOP (max 8 Hops),
lstat S_IFLNK, O_NOFOLLOW, unlink Symlink. 25 Tests.

Noch offen:
- [ ] ext2: rwx Bits korrekt enforced (chmod)

### Job Control — erledigt (EQ-C)

### Parallele Test-Ausfuehrung — erledigt

- [x] test/main.c: fork pro Test, BATCH=4 gleichzeitig
- [x] Jeder Test in eigenem Prozess (Isolation via fork + setsid)
- [x] MAP_SHARED Slots fuer Ergebnis-IPC (pass_cnt, fail_cnt, done)
- [x] Parent: wait4 Loop pro Batch, sammelt PASS/FAIL aus Slots
- [x] Kernel-Fix: exit_group orphaned Threads (proc=NULL + sched drain)
- [x] Kernel-Fix: fork erbt is_driver + cmdline

### Boot-Config — /etc/cosmo.conf

- [ ] config_load/config_get/config_get_int Parser
- [ ] Keys: keymap, vt.color, vt.font, vt.scrollback, vt.0-3, net.hostname
- [ ] Kernel Command-Line als Fallback

---

## Alpine Bootstrap — Weg zum Self-Hosting

Userland = Alpine Linux (musl-nativ). Kein eigenes Userland.

### Phase 1: Busybox Shell (statisch) — erledigt

Busybox 1.36.1 (musl-gcc, statisch, 1.17MB). Eingebettet als /bin/sh in ramfs.
init.c execve'd /bin/sh → ash Prompt. AT_PHDR Fix (prog_phdr aus PT_LOAD vaddr).
ELF-Loader Refactoring: 4→1 Variante, CosmoFS→ext2 migriert, crt0.S ABI-korrekt.
make qemu-busybox zum Testen.

### Phase 2: Kernel-Features fuer Alpine — erledigt

Symlinks: komplett (ramfs + ext2, path resolution, ELOOP, 25 Tests).
futex: Spin-Wait → event_wait/event_post, Timeouts, PI, WAKE_OP, 6 Tests.

Noch offen (nicht blockierend fuer Phase 3):
- [ ] /etc/resolv.conf lesbar fuer musl DNS-Resolver
- [ ] /dev/pts: posix_openpt / openpty (Terminal-Multiplexing)

### Phase 3: Alpine Base — erledigt

Alpine minirootfs bootet von ext2. Dynamic Linking (ld-musl-x86_64.so.1).
Interaktive busybox ash Shell: echo, ls, cat, uname funktionieren.
PTY/termios: TCGETS/TCSETS real, pty_input_direct (DSR bypass), poll()-Wake
fuer PTY-Input, timer-getriebener vt_flush, /dev/console.
make qemu-alpine bootet Alpine von ext2 Disk-Image.

Noch offen:
- [ ] ash crasht nach externem Kommando im interaktiven Modus (Signal-Frame?)
- [ ] Fuzz-Crash: seed=0x2a057240, Round 8 (Agent debuggt)
- [ ] apk-tools lauffaehig (apk add/del/update)
- [ ] test: apk add bash && bash --version

### Phase 4: Entwicklungsumgebung

- [ ] apk add gcc musl-dev make
- [ ] test: gcc -o hello hello.c && ./hello
- [ ] apk add git
- [ ] test: git clone funktioniert

### Phase 5: Self-Hosting (Ziel)

- [ ] apk add nodejs npm
- [ ] npm install -g @anthropic-ai/claude-code
- [ ] test: claude --version
- [ ] CosmoRT Kernel auf CosmoRT kompilieren

---

## TCP Advanced — erledigt

Timestamps (RFC 7323): RTTM + PAWS. ECN (RFC 3168): CWR/ECE + IP ECT(0).
TFO (RFC 7413): 64-Entry Cookie Cache, Client SYN mit Cookie.

---

## Security Hardening

### Sec-A: ASLR — erledigt

Stack 22-bit, mmap 28-bit, PIE 28-bit, Heap 13-bit, KASLR 9-bit.

### Sec-B: SMEP + SMAP — erledigt

CR4 Bits 20+21 auf BSP+AP, STAC/CLAC in Syscall-Entry + Signal-Frame.

### Sec-C: W^X — erledigt

mmap/mprotect(WRITE|EXEC) → -EINVAL.

### Sec-D: Stack Guard Pages — erledigt

PROT_NONE Guard Page unter jedem User-Stack.

### Sec-E: Spectre/Meltdown

#### Sec-E1: KPTI (Kernel Page Table Isolation)

- [ ] Separate Kernel/User Page Tables (pro Prozess)
- [ ] SYSCALL/SYSRET Switch: User-PML4 hat nur Trampoline-Page, kein Kernel-Mapping
- [ ] Interrupt-Entry: switch zu Kernel-PML4 vor Handler
- [ ] PCID fuer TLB-Performance (vermeidet Flush bei jedem Switch)
- [ ] test: User-Code kann Kernel-Adressen nicht lesen

#### Sec-E2: Retpoline + IBRS

- [ ] -mindirect-branch=thunk in CFLAGS (GCC Retpoline)
- [ ] IBRS (Indirect Branch Restricted Speculation): IA32_SPEC_CTRL MSR
- [ ] IBPB (Indirect Branch Prediction Barrier): bei Context-Switch
- [ ] STIBP (Single Thread Indirect Branch Predictors): bei SMT

#### Sec-E3: SSBD + MDS

- [ ] SSBD (Speculative Store Bypass Disable): IA32_SPEC_CTRL Bit 2
- [ ] MDS Mitigations: VERW bei Kernel-Entry (MD_CLEAR)
- [ ] test: Spectre-v1/v2 PoC laeuft nicht

---

## Offen — Netzwerk

### IPv6

#### IPv6-A: Basis (RFC 8200)

- [ ] IPv6 Header Parsing + Generierung (128-bit Adressen)
- [ ] Next Header Chain (Extension Headers)
- [ ] AF_INET6 Socket-Familie
- [ ] Dual-Stack: AF_INET6 Socket akzeptiert IPv4 (::ffff:a.b.c.d)
- [ ] test: ping6 localhost

#### IPv6-B: NDP (RFC 4861)

- [ ] Neighbor Solicitation / Advertisement (ersetzt ARP)
- [ ] Router Solicitation / Advertisement
- [ ] Neighbor Cache (analog zu ARP Cache)
- [ ] test: IPv6 Neighbor Discovery funktioniert

#### IPv6-C: SLAAC (RFC 4862)

- [ ] Stateless Address Auto-Configuration
- [ ] Link-Local Address (fe80::)
- [ ] Global Address aus Router Advertisement
- [ ] DAD (Duplicate Address Detection)
- [ ] test: automatische IPv6-Adresse nach Boot

---

## Offen — io_uring (Node.js 22+ Performance)

### io_uring-A: Kern-Infrastruktur

- [ ] Shared Ring-Buffer: Submission Queue (SQ) + Completion Queue (CQ)
- [ ] io_uring_setup: alloziert SQ/CQ, gibt fd zurueck
- [ ] io_uring_enter: submitted Entries verarbeiten, auf Completions warten
- [ ] io_uring_register: Buffers/Files registrieren
- [ ] mmap fuer SQ/CQ Ring-Pages (User-Shared Memory)

### io_uring-B: Operationen

- [ ] IORING_OP_READV / WRITEV: File I/O
- [ ] IORING_OP_READ / WRITE: vereinfachte Variante
- [ ] IORING_OP_OPENAT / CLOSE: File-Lifecycle
- [ ] IORING_OP_ACCEPT / CONNECT: Socket I/O
- [ ] IORING_OP_SEND / RECV: Netzwerk
- [ ] IORING_OP_TIMEOUT: Timer
- [ ] IORING_OP_POLL_ADD / REMOVE: Event-Polling

### io_uring-C: Performance

- [ ] SQPOLL: Kernel-Thread pollt SQ (kein Syscall pro Submit)
- [ ] Fixed Buffers: vorab-registrierte I/O Buffer
- [ ] Fixed Files: vorab-registrierte FDs
- [ ] test: Node.js 22 I/O Benchmark laeuft

---

## Offen — Moderne Syscalls (Rest)

- [ ] pidfd_open + pidfd_send_signal + pidfd_getfd
- [ ] SO_REUSEPORT, MSG_ZEROCOPY

---

## Offen — Device Nodes fuer SDL3/UI

SDL3 laeuft unmodifiziert ueber Standard Linux Device Nodes.
Kein Custom-Backend, kein SDL3-Fork. apk add sdl3 funktioniert direkt.

### /dev/fb0 (Framebuffer)

- [ ] /dev/fb0 Device Node: open, mmap (Pixel-Buffer), ioctl (FBIOGET_VSCREENINFO etc.)
- [ ] Pro VT-Slot ein Framebuffer (12 Slots, F1-F12)
- [ ] SDL3 fbdev Backend: test mit SDL3 Hello Window

### /dev/input/event0 (evdev)

- [ ] /dev/input/eventN Device Nodes: read → struct input_event
- [ ] Keyboard, Maus, Gamepad Events im Linux evdev-Format
- [ ] SDL3 evdev Backend: test mit SDL3 Input

### /dev/snd/ (ALSA)

- [ ] /dev/snd/pcmC0D0p: PCM Playback Device
- [ ] ALSA ioctl Subset (SNDRV_PCM_IOCTL_*)
- [ ] 12-Spur Mixer im RT-Core (ein Stream pro VT-Slot)
- [ ] SDL3 ALSA Backend: test mit SDL3 Audio

### /dev/dri/ (GPU, spaeter)

- [ ] virtio-gpu Treiber vollstaendig
- [ ] /dev/dri/card0: KMS/DRM ioctl Subset
- [ ] SDL3 KMS/DRM Backend: OpenGL/Vulkan

---

## Offen — VT/Terminal

### VT-A: Scrollback-Buffer

- [ ] Ringbuffer 4096 Zeilen, Shift+PageUp/Down

### VT-B: Dirty-Line Tracking

- [ ] Dirty-Bitmap pro Zeile, nur dirty Zeilen rendern

### VT-C: Keymaps + AltGr

- [ ] .keymap Dateien auf ext2, AltGr, DE-QWERTZ

### VT-D: Alternate Screen

- [ ] CSI ?1049h/l (less, vim, htop)

### Font-A: Zweistufiger Glyph-Lookup

- [ ] glyph_fast[256] O(1), binaere Suche nur fuer U+0100+

### Font-B: Font aus ramfs

- [ ] .font Binaerformat (Header + Glyph-Map + Bitmap)
- [ ] tools/mkfont.py: generiert benannte .font Dateien (z.B. iosevka-19.font)
- [ ] Font-Verzeichnis: /lib/fonts/*.font
- [ ] /etc/cosmo.conf: vt.font = /lib/fonts/iosevka-19.font (konfigurierbar)
- [ ] fb_init(): laedt Font aus config-Pfad, Fallback auf ersten .font in /lib/fonts/

---

## Offen — Skalierung (niedrige Prio)

Skal-G (PTY Pool), Skal-H (epoll_ctl Hash), Skal-I (Unix Socket Slab),
Skal-J (IPC Free-List), Skal-K (procfs Bitmap), Skal-L (epoll Sleeper List),
Skal-M (PROC_MAX dynamisch), Skal-N (Event-Pool Slab).

---

## Offen — RT/Compute

### RT/Compute-F: Core-Isolation — erledigt

sched_add: SCHED_OTHER nur Core 1..N, nie Core 0. Isolation-Fallback scannt Compute-Cores.

### RT/Compute-G: Timer + IPI Preemption — erledigt

RT-Core 1000Hz (1ms), Compute 100Hz (10ms). IPI → sched_preempt direkt. Wake <1ms.

### ARINC 653 Partitionierung

- [ ] ARINC-A: Shared Locks eliminieren
- [ ] ARINC-B: Dynamic Alloc auf RT-Core eliminieren
- [ ] ARINC-C: IPI-Isolation
- [ ] ARINC-D: Bounded Data Structures
- [ ] ARINC-E: RT-Core Memory-Isolation



### SHA-NI Upgrade (CPUID Check, sha256rnds2)

---

## Offen — Observability

/proc/pid/maps, /proc/pid/status, /proc/pid/cmdline, /proc/meminfo: erledigt.
- [ ] perf_event_open (geplant)

---

## Offen — Fehlende Implementierungen (aus Source-Kommentaren)

### Syscalls/Prozesse

- [ ] ARCH_SET_GS / ARCH_GET_GS: GS-Base Management (sys_proc.c:18)
- [ ] vfork: echte Semantik (Parent suspended bis exec), aktuell Alias auf fork
- [ ] prlimit64: set-Operationen ignoriert (sys_proc.c:489)
- [ ] sendfile: nicht implementiert, return -ENOSYS (stubs.c:19)
- [ ] MREMAP_FIXED: return -EINVAL (sys_mem.c:872)

### Dateisystem

- [ ] Hard Links in ext2 (vfs_dirops.c:164)
- [ ] renameat2 RENAME_EXCHANGE (sys_fs.c:53)
- [ ] futimens auf FD (sys_fs.c:147, aktuell no-op)
- [ ] B+ Tree Rebalancing nach Delete (btree.c:589)
- [ ] /proc/pid/maps: shared/private korrekt anzeigen (procfs.c:231)

### IPC/Synchronisation — erledigt

futex: echtes Blocking via event_wait/event_post, Timeouts (timespec→ms),
PI Boost/Unboost, FUTEX_WAKE_OP, CMP_REQUEUE, WAIT_BITSET. 6 Tests.

### Netzwerk

- [ ] AF_UNIX shutdown: aktuell no-op (socket.c:820)

### Events

- [ ] inotify: nur Stub, keine echten FS-Events (inotify.c)

### Hardware/Treiber

- [ ] MMIO Pages: PTE_PCD setzen (cosmort.c:67)
- [ ] Virtio Feature-Negotiation: nur 32-Bit (virtio.c:264)
- [ ] Hyper-V Framebuffer: VMBus Protokoll, VRAM Mapping (hyperv_fb.c:103)
- [ ] Hyper-V Mouse: HID Descriptor Parsing, Event Loop (hv_mouse.c:92)

### RT/Audio

- [ ] Audio/Input/VSync Poll Handler: nur Stubs (rt_poll.c:31)

---

## Offen (sonstige)

- [ ] c-ares UDP DNS (ETIMEOUT trotz korrekter Pakete)
- [ ] GPT-Image Boot (Partitions-Support)
