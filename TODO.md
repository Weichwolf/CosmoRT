# CosmoRT — Offene Punkte

Stand: 2026-03-29. 1201 ktest PASS. Node.js + gcc + npm auf CosmoRT.

---

## Alpine Kompatibilitaet — Blocker

### IPC

- [ ] inotify echte Events (IN_CREATE, IN_MODIFY, IN_DELETE) — Node.js fs.watch, npm
- [ ] SCM_RIGHTS: fd-Passing ueber Unix Socket (D-Bus, viele Daemons)
- [ ] Abstract Unix Sockets (@ Namespace, D-Bus)
- [ ] /dev/shm: POSIX Shared Memory (shm_open/shm_unlink → tmpfs auf /dev/shm)
- [ ] flock echtes Advisory Locking (nicht nur return 0)
- [ ] SysV IPC: shmget/shmat/shmctl, semget/semop/semctl, msgget/msgsnd/msgrcv/msgctl

### Prozesse/Threads

- [ ] CLONE_THREAD korrekt: echte Threads (gleiche PID, verschiedene TIDs, shared fd_table)
- [ ] sendfile: zero-copy file transfer (nginx, manche npm Operationen)
- [ ] getrusage korrekt: CPU-Zeit pro Prozess/Thread

### procfs

- [ ] /proc/self/fd/ Directory Listing (readdir)
- [ ] /proc/pid/stat komplett (alle Felder, nicht nur Basis)
- [ ] /proc/sys/kernel/threads-max, /proc/sys/vm/overcommit_memory

### Terminal (siehe notes/TERMINAL.md)

- [x] Volle termios Speicherung (c_iflag, c_oflag, c_cflag, c_lflag, c_cc[19])
- [x] ICRNL (CR→NL Input-Translation)
- [x] OPOST+ONLCR (NL→CR+NL Output-Translation)
- [x] Konfigurierbare Control Characters aus c_cc[] (VINTR, VQUIT, VSUSP, VEOF, VERASE, VKILL, VWERASE)
- [x] ISIG-Flag steuert Signal-Generierung
- [x] /dev/tty1-tty4, /dev/console, /dev/pts/0-3 Device Nodes
- [ ] VMIN/VTIME fuer Raw-Mode Timeout-Reads
- [ ] SIGTTIN/SIGTTOU fuer Background Terminal-Zugriff
- [ ] Interaktive Shell: ash crasht nach externem Kommando
- [ ] Restructure: src/kernel/vt/ → src/kernel/tty/ (siehe notes/TERMINAL.md)

---

## Security

### Sec-E: Spectre/Meltdown

#### Sec-E1: KPTI

- [ ] Separate Kernel/User Page Tables
- [ ] PCID fuer TLB-Performance

#### Sec-E2: Retpoline + IBRS

- [ ] -mindirect-branch=thunk in CFLAGS
- [ ] IBRS/IBPB bei Context-Switch
- [ ] STIBP bei SMT

#### Sec-E3: SSBD + MDS

- [ ] SSBD: IA32_SPEC_CTRL Bit 2
- [ ] MDS: VERW bei Kernel-Entry

---

## Netzwerk

### IPv6

#### IPv6-A: Basis (RFC 8200)

- [ ] IPv6 Header, AF_INET6, Dual-Stack
- [ ] Next Header Chain

#### IPv6-B: NDP (RFC 4861)

- [ ] Neighbor Solicitation/Advertisement
- [ ] Router Solicitation/Advertisement

#### IPv6-C: SLAAC (RFC 4862)

- [ ] Stateless Address Auto-Configuration
- [ ] Link-Local + Global Address + DAD

---

## io_uring (Node.js Performance)

### io_uring-A: Kern-Infrastruktur

- [ ] SQ/CQ Shared Ring-Buffer, io_uring_setup/enter/register
- [ ] mmap fuer Ring-Pages

### io_uring-B: Operationen

- [ ] READV/WRITEV, READ/WRITE, OPENAT/CLOSE
- [ ] ACCEPT/CONNECT, SEND/RECV, TIMEOUT, POLL

### io_uring-C: Performance

- [ ] SQPOLL Kernel-Thread
- [ ] Fixed Buffers/Files

---

## Device Nodes (SDL3/UI)

### /dev/fb0 (Framebuffer)

- [ ] open, mmap (Pixel-Buffer), ioctl (FBIOGET_VSCREENINFO etc.)
- [ ] Pro VT-Slot ein Framebuffer

### /dev/input/event0 (evdev)

- [ ] read → struct input_event (Keyboard, Maus, Gamepad)

### /dev/snd/ (ALSA)

- [ ] PCM Playback Device, ALSA ioctl Subset
- [ ] 12-Spur Mixer im RT-Core

### /dev/dri/ (GPU)

- [ ] virtio-gpu vollstaendig, KMS/DRM ioctl

---

## VT/Terminal (siehe notes/TERMINAL.md)

### VT-A: Scrollback-Buffer

- [ ] Ringbuffer 4096 Zeilen, Shift+PageUp/Down

### VT-B: Keymaps + AltGr

- [ ] .keymap Dateien, AltGr, DE-QWERTZ

### VT-C: Alternate Screen

- [ ] CSI ?1049h/l (less, vim, htop)

### Font

- [ ] Zweistufiger Glyph-Lookup: glyph_fast[256] O(1), binaere Suche U+0100+
- [ ] Font aus ext2: .font Binaerformat, /lib/fonts/, konfigurierbar

---

## RT/Compute

### ARINC 653 Partitionierung

- [ ] ARINC-A: Shared Locks eliminieren
- [ ] ARINC-B: Dynamic Alloc auf RT-Core eliminieren
- [ ] ARINC-C: IPI-Isolation
- [ ] ARINC-D: Bounded Data Structures
- [ ] ARINC-E: RT-Core Memory-Isolation

---

## Skalierung (niedrige Prio)

Skal-G (PTY Pool), Skal-H (epoll_ctl Hash), Skal-I (Unix Socket Slab),
Skal-J (IPC Free-List), Skal-K (procfs Bitmap), Skal-L (epoll Sleeper List),
Skal-M (PROC_MAX dynamisch), Skal-N (Event-Pool Slab).

---

## Fehlende Implementierungen

### Memory

- [ ] pages_alloc Caller Review: wer nutzt grosse Orders fuer Userspace?
- [ ] MREMAP_FIXED: vollstaendig (Agent hat es implementiert — verifizieren)

### Syscalls

- [ ] ARCH_SET_GS / ARCH_GET_GS: GS-Base Management
- [ ] prlimit64: set-Operationen
- [ ] sendfile: echte Implementation
- [ ] splice/tee/vmsplice: zero-copy Pipe Operationen

### Dateisystem

- [ ] Hard Links in ext2
- [ ] renameat2 RENAME_EXCHANGE
- [ ] ext2 Audit: Korrektheit gegen Spec (siehe notes/PLAN_SYSCALL_AUDIT.md)

### Hardware

- [ ] MMIO Pages: PTE_PCD setzen
- [ ] Virtio Feature-Negotiation: 64-Bit
- [ ] Hyper-V Framebuffer + Mouse
