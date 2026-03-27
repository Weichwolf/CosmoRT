# CosmoRT — Offene Punkte

Stand: 2026-03-26. 1091 ktest PASS, 0 FAIL.

---

## Erledigt (Zusammenfassung)

Net Phase 0-E: TCP/UDP/ARP/IP/DNS/DHCP extrahiert, CUBIC (RFC 8312), SACK (RFC 2018),
Window Scaling (RFC 7323), OOO Buffering, Fast Retransmit/Recovery, Keepalive,
EINPROGRESS, SO_RCVTIMEO, Sleep/Wake statt Polling, TX-Ring.

RT/Compute A-E: SPSC Channels, IPI+Wake, TX-Ring, Timer-Wheel, Priorisierte Polling-Schleife.

G1-G2: SHA-256 Hash-Engine auf RT-Core, KSM RAM-Dedup mit Page-Age Tracking.

MM: COW fork, MADV_FREE, Transparent Huge Pages (2MB).

Skalierung A-F: Socket Slab, TCP Hash Chaining, UDP Hash+Pool, ARP Hash+Pool,
Timer Free-Stack, FD Bitmap. Alles O(1).

SH-B: Device Nodes (/dev/null, /dev/zero, /dev/urandom, /dev/tty).

Security: 5 Fuzz-Fixes (fault_recover, sigreturn MXCSR, signal handler validation,
range checks, mlock DoS).

Restrukturierung: include/linux/, include/kernel/ (Subsystem-Spiegel), src/kernel/sys/,
arch/x86_64/, drivers nach Bus sortiert, event split, arch.h Abstraktion.
Phase 1 File-Split: vfs.c→6, process.c→5, sys_signal.c→3 (4495→14 Dateien + 2 interne Header).

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

### SH-H: Symlinks + Permissions

- [ ] Symlink-Resolution in allen Pfad-Operationen
- [ ] CosmoFS: rwx Bits speichern (chmod), nur +x enforced
- [ ] test: Symlink-Chain 3 Ebenen, chmod +x → exec

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

### Phase 1: Busybox Shell (statisch)

- [ ] busybox mit musl-gcc statisch kompilieren
- [ ] Als Init booten → interaktive Shell
- [ ] Grundlegende Befehle testen: ls, cat, echo, mkdir, cp

### Phase 2: Kernel-Features fuer Alpine

- [ ] SH-H: Symlinks (busybox/Alpine braucht /bin/sh → busybox etc.)
- [ ] futex echtes Blocking + Timeouts (pthreads, jedes Multi-Thread-Programm)
- [ ] /etc/resolv.conf lesbar fuer musl DNS-Resolver
- [ ] /dev/pts: posix_openpt / openpty (Terminal-Multiplexing)

### Phase 3: Alpine Base

- [ ] ld-musl-x86_64.so.1 einbetten (Dynamic Linker)
- [ ] Alpine minirootfs extrahieren → CosmoFS oder ramfs
- [ ] apk-tools lauffaehig (apk add/del/update)
- [ ] /etc/passwd, /etc/group (getpwnam, getgrgid)
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

## Offen — TCP Advanced

- [ ] TCP Timestamps (RFC 7323): RTTM + PAWS
- [ ] ECN (RFC 3168): IP ECN Bits + TCP CWR/ECE
- [ ] TFO (TCP Fast Open, RFC 7413): Daten im SYN, Cookie-Management

---

## Offen — Security Hardening

### Sec-A: ASLR

- [ ] Stack/mmap/PIE/Heap/KASLR Randomisierung
- [ ] test: zwei execve → unterschiedliche Adressen

### Sec-B: SMEP + SMAP

- [ ] CR4.SMEP=1, CR4.SMAP=1 bei Boot
- [ ] STAC/CLAC um copy_from_user/copy_to_user

### Sec-C: W^X Enforcement

- [ ] mmap(PROT_WRITE|PROT_EXEC) = -EINVAL
- [ ] Kernel .text RX, .data RW, nie beides

### Sec-D: Stack Guard Pages

- [ ] Unmapped Page am Stack-Ende, Thread-Stacks

### Sec-E: Spectre/Meltdown

- [ ] Retpoline, IBPB, KPTI (geplant)

---

## Offen — Netzwerk

### IPv6

- [ ] RFC 8200, RFC 4861 (NDP), RFC 4862 (SLAAC)
- [ ] Dual-Stack, AF_INET6

---

## Offen — Moderne Syscalls

- [ ] io_uring (Node.js 22+)
- [ ] memfd_create (Chrome, Wayland)
- [ ] copy_file_range, close_range
- [ ] pidfd_open + pidfd_send_signal
- [ ] SO_REUSEPORT, MSG_ZEROCOPY

---

## Offen — VT/Terminal

### VT-A: Scrollback-Buffer

- [ ] Ringbuffer 4096 Zeilen, Shift+PageUp/Down

### VT-B: Dirty-Line Tracking

- [ ] Dirty-Bitmap pro Zeile, nur dirty Zeilen rendern

### VT-C: Keymaps + AltGr

- [ ] .keymap Dateien auf CosmoFS, AltGr, DE-QWERTZ

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

### RT/Compute-F: Skalierung

- [ ] RT_CORE_COUNT Abstraktion, kein hardcoded Core 0

### ARINC 653 Partitionierung

- [ ] ARINC-A: Shared Locks eliminieren
- [ ] ARINC-B: Dynamic Alloc auf RT-Core eliminieren
- [ ] ARINC-C: IPI-Isolation
- [ ] ARINC-D: Bounded Data Structures
- [ ] ARINC-E: RT-Core Memory-Isolation

### G3: CosmoFS Block-Dedup (Abhaengigkeit: CosmoFS v2)

### G4: Cloud-Sync Hash-Readiness (Abhaengigkeit: CosmoFS v2)

### SHA-NI Upgrade (CPUID Check, sha256rnds2)

---

## Offen — Observability

- [ ] /proc/pid/maps, /proc/pid/status, /proc/pid/cmdline
- [ ] /proc/meminfo
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

- [ ] Hard Links in CosmoFS (vfs_dirops.c:164)
- [ ] renameat2 RENAME_EXCHANGE (sys_fs.c:53)
- [ ] futimens auf FD (sys_fs.c:147, aktuell no-op)
- [ ] CosmoFS Triple-Indirect Blocks: grosse Dateien (cosmofs.c:398)
- [ ] CosmoFS xattr/Extended Attributes (cosmofs.c:683)
- [ ] B+ Tree Rebalancing nach Delete (btree.c:589)
- [ ] /proc/pid/maps: shared/private korrekt anzeigen (procfs.c:231)

### IPC/Synchronisation

- [ ] futex Timeouts: timeout-Parameter ignoriert (futex.c:278)
- [ ] futex echtes Blocking statt Spin-Wait (futex.c:156)

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
