# CosmoRT Design Specification

Spec-Tree. Definiert korrektes Verhalten durch externe Standards + Architektur-Entscheidungen.
Code wird gegen dieses Dokument verifiziert. Abweichungen sind Bugs.

Keine Implementierungsdetails — die stehen im Code.

---

## 1. CPU / Architecture

### 1.1 x86_64 Baseline

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel SDM Vol 1-4 | Intel 64 and IA-32 Architectures Software Developer's Manual | Paging, Exceptions, APIC, MSRs, TSC |
| AMD64 Architecture Programmer's Manual | AMD Vol 1-5 | Equivalent, AMD-spezifische MSRs |
| System V ABI AMD64 | https://gitlab.com/x86-psABIs/x86-64-ABI | Calling Convention, Stack Layout, Register Usage |

Entscheidung: x86_64 only (ARM64/RISC-V geplant via src/arch/).

### 1.2 Paging

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel SDM Vol 3 Chapter 4 | 4-Level Paging (PML4) | 4KB Pages + 2MB Huge Pages (PS-Bit) |
| Intel SDM Vol 3 §4.6 | Access/Dirty Bits | Page-Age Tracking (KSM) |
| Intel SDM Vol 3 §4.7 | Page-Fault Exception (#PF) | COW, Demand-Paging, THP |

PTE Software-Bits:
- Bit 9: PTE_COW (Copy-on-Write)
- Bit 10: PTE_LAZYFREE (MADV_FREE reclaimable)

### 1.3 Interrupts / APIC

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel SDM Vol 3 Chapter 10 | Local APIC | Timer (Periodic, Vector 32), EOI, ICR (IPI) |
| Intel SDM Vol 3 §10.6 | I/O APIC | IRQ Routing (alle auf Core 0) |
| Intel SDM Vol 3 §10.12 | IPI (Inter-Processor Interrupt) | rt_wake: Vector 0xFD, Fixed Delivery |

Entscheidung: Alle IRQs auf Core 0 (RT-Core). Kein IRQ-Balancing.

### 1.4 SIMD / FPU

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel SDM Vol 1 Chapter 10 | SSE/SSE2 | Userspace + RT-Core Hash-Engine |
| Intel SDM Vol 1 §10.5 | FXSAVE/FXRSTOR (512 Bytes) | Context Switch, fork, Signal |
| Intel SDM Vol 2 | MXCSR Register | Init 0x1F80, sanitized bei sigreturn |

Entscheidung: SSE2 maximum (WASM SIMD Limit). Kein AVX/AVX-512, kein XSAVE.
Kernel: -mno-sse (außer sha256.c). RT-Core: SSE frei (kein User-FPU).

### 1.5 CPU Security Features

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel SDM Vol 3 §4.6 | SMEP (Supervisor Mode Execution Prevention) | CR4.SMEP=1: Kernel kann keinen User-Code ausfuehren |
| Intel SDM Vol 3 §4.6 | SMAP (Supervisor Mode Access Prevention) | CR4.SMAP=1: Kernel kann keine User-Pages lesen/schreiben (außer explizit) |
| Intel SDM Vol 3 §6.15 | NX/XD Bit | No-Execute: Daten-Pages nicht ausfuehrbar |

Entscheidung: SMEP + SMAP + NX mandatory bei Boot. Fehlen = Panic.

### 1.6 TSC / Timer

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel SDM Vol 3 §17.17 | Time-Stamp Counter (TSC) | timer_ms(), Calibration via LAPIC |
| Intel SDM Vol 3 §10.5.4 | LAPIC Timer | Periodic, Divider 16, Init 10000000 |

---

## 2. Boot

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| UEFI Specification 2.10 | https://uefi.org/specifications | Boot Services, GOP, Memory Map, Exit Boot Services |
| ACPI Specification 6.5 | https://uefi.org/specifications | PM1a_CNT (Shutdown), S5 Sleep State |
| GNU-EFI | https://sourceforge.net/projects/gnu-efi/ | EFI Application Build |

Entscheidung: UEFI-only (kein Legacy BIOS). GPT Partitioning.

---

## 3. Linux ABI

### 3.1 Syscall ABI

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Linux syscall table x86_64 | arch/x86/entry/syscalls/syscall_64.tbl | Nr 0-471 + CosmoRT 0x10000+ |
| Linux syscall convention | man 2 syscall | rax=nr, rdi/rsi/rdx/r10/r8/r9=args, rax=return |
| Linux errno | include/uapi/asm-generic/errno.h | EPERM(1) bis ECONNREFUSED(111) |

Entscheidung: Exakt Linux x86_64 ABI. ELF-Binaries laufen unveraendert.

### 3.2 ELF

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| ELF Specification (TIS) | Tool Interface Standard, Portable Formats | ET_EXEC, ET_DYN, PT_LOAD, PT_INTERP, PT_PHDR |
| Linux ELF Auxiliary Vector | include/uapi/linux/auxvec.h | AT_PHDR, AT_ENTRY, AT_BASE, AT_PAGESZ |

### 3.3 Signals

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 §2.4 | Signal Concepts | 31 Standard-Signale |
| Linux sigaction | man 2 rt_sigaction | SA_RESTART, SA_SIGINFO, SA_NOCLDSTOP |
| Linux signal frame x86_64 | arch/x86/kernel/signal.c | ucontext_t, siginfo_t, FXSAVE in fpstate |

### 3.4 Process

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 fork/exec/wait | IEEE 1003.1 | fork, execve, wait4, exit_group |
| Linux clone | man 2 clone | CLONE_VM, CLONE_FS, CLONE_FILES, CLONE_THREAD |
| Linux clone3 | man 2 clone3 | struct clone_args |
| Linux prctl | man 2 prctl | PR_SET_NAME, PR_SET_PDEATHSIG |
| Linux arch_prctl | man 2 arch_prctl | ARCH_SET_FS, ARCH_SET_GS (TLS) |

### 3.5 Memory

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 mmap | IEEE 1003.1 | MAP_PRIVATE, MAP_ANONYMOUS, MAP_FIXED |
| Linux mmap | man 2 mmap | MAP_SHARED (TODO), MAP_NORESERVE |
| Linux madvise | man 2 madvise | MADV_DONTNEED, MADV_FREE |
| Linux brk | man 2 brk | Heap Management |
| Linux mlock | man 2 mlock | mlockall(MCL_CURRENT\|MCL_FUTURE) |

### 3.6 File I/O

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 File I/O | IEEE 1003.1 | open, read, write, close, lseek, stat |
| Linux openat | man 2 openat | AT_FDCWD, O_CLOEXEC |
| Linux *at Syscalls | man 2 openat, mkdirat, etc. | Legacy delegiert an *at-Varianten |
| Linux fcntl | man 2 fcntl | F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL |
| Linux ioctl | man 2 ioctl | TIOCGWINSZ, TIOCSWINSZ, TIOCSPGRP (TODO) |
| Linux getdents64 | man 2 getdents64 | Directory Iteration |
| Linux statx | man 2 statx | Extended stat |

### 3.7 IPC

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 Pipes | IEEE 1003.1 | pipe, pipe2 |
| Linux futex | man 2 futex | FUTEX_WAIT, FUTEX_WAKE |
| Linux eventfd | man 2 eventfd2 | EFD_NONBLOCK, EFD_CLOEXEC |
| Linux timerfd | man 2 timerfd_create | CLOCK_MONOTONIC, CLOCK_REALTIME |
| Linux signalfd | man 2 signalfd4 | Signal als fd |
| Linux inotify | man 2 inotify_init1 | Filesystem Monitoring |

### 3.8 Event Polling

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Linux epoll | man 7 epoll | epoll_create1, epoll_ctl, epoll_wait, EPOLLIN/OUT/ET |
| Linux poll | man 2 poll | POLLIN, POLLOUT, POLLERR |
| Linux select/pselect | man 2 pselect6 | Kompatibilitaet |

### 3.9 Sockets

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 Sockets | IEEE 1003.1 | socket, bind, listen, accept, connect |
| Linux socket | man 2 socket | AF_INET, AF_UNIX, SOCK_STREAM, SOCK_DGRAM |
| Linux sendmsg/recvmsg | man 2 sendmsg | struct msghdr, AF_UNIX/AF_INET Dispatch |
| Linux setsockopt | man 2 setsockopt | SO_REUSEADDR, SO_REUSEPORT, SO_KEEPALIVE, TCP_NODELAY |
| Linux MSG_ZEROCOPY | man 7 socket | Zero-Copy Send (kein memcpy im TX-Pfad) |

### 3.10 Time

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 Clocks | IEEE 1003.1 | clock_gettime, clock_getres |
| Linux clock IDs | man 2 clock_gettime | CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME |
| Linux nanosleep | man 2 nanosleep | High-Resolution Sleep |

### 3.11 Scheduling

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 Scheduling | IEEE 1003.1 | sched_yield, sched_setaffinity |
| Linux SCHED_OTHER/FIFO/RR | man 7 sched | CFS-equivalent auf Compute-Cores |

### 3.12 System

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Linux uname | man 2 uname | sysname="CosmoRT" |
| Linux sysinfo | man 2 sysinfo | Uptime, RAM, Processes |
| Linux getrandom | man 2 getrandom | GRND_NONBLOCK |
| Linux reboot | man 2 reboot | LINUX_REBOOT_CMD_POWER_OFF, RESTART, HALT |

---

## 4. Netzwerk Protokolle

### 4.1 IP

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 791 | Internet Protocol | IPv4 Header, Checksum, TTL |
| RFC 1071 | Computing the Internet Checksum | One's Complement Sum |
| RFC 1122 | Requirements for Internet Hosts | Host Requirements |
| RFC 8200 | Internet Protocol Version 6 | IPv6 Header, Extension Headers |
| RFC 4861 | Neighbor Discovery for IPv6 | NDP (replaces ARP for IPv6) |
| RFC 4862 | IPv6 Stateless Address Autoconfiguration | SLAAC |

Entscheidung: Dual-Stack (IPv4 + IPv6).

### 4.2 TCP

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 793 | Transmission Control Protocol | 10-State Machine, SEQ/ACK, Flow Control |
| RFC 8312 | CUBIC Congestion Control | W(t)=C×(t-K)³+Wmax, Beta=0.7, Integer-Arithmetik |
| RFC 5681 | TCP Congestion Control | Slow-Start, Fast Retransmit §3.2, Fast Recovery |
| RFC 6928 | Increasing TCP Initial Window | IW=10 (14600 Bytes) |
| RFC 2018 | TCP Selective Acknowledgment (SACK) | SACK Permitted + Blocks, 4 Slots |
| RFC 7323 | TCP Extensions (Window Scaling, Timestamps) | WScale Shift=7, Timestamps fuer RTTM + PAWS |
| RFC 3168 | Explicit Congestion Notification (ECN) | IP ECN Bits + TCP CWR/ECE Flags |
| RFC 7413 | TCP Fast Open (TFO) | Daten im SYN, Cookie-Management |
| RFC 6298 | Computing TCP Retransmission Timer | RTO Calculation |
| RFC 1122 §4.2 | TCP Requirements | Keepalive (75s, 9 Probes) |

Entscheidung: Kein Nagle (immer TCP_NODELAY-Verhalten).

### 4.3 UDP

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 768 | User Datagram Protocol | Stateless Send/Recv |

### 4.4 ARP

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 826 | Address Resolution Protocol | Request/Reply, Cache (128 Entries) |

### 4.5 ICMP

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 792 | Internet Control Message Protocol | Echo Reply (Kernel), Echo Request (Userspace) |

### 4.6 DHCP

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 2131 | Dynamic Host Configuration Protocol | DISCOVER/OFFER/REQUEST/ACK |
| RFC 2132 | DHCP Options | Option 3 (Gateway), Option 6 (DNS) |

### 4.7 DNS

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| RFC 1035 | Domain Names — Implementation | A-Record Query, Response Parsing, Name Compression |

Entscheidung: Kernel-DNS nur fuer Boot. Userspace-DNS fuer alles andere.

---

## 5. Crypto

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| FIPS 180-4 | Secure Hash Standard (SHA-256) | Block-Hashing, KSM, CosmoFS Dedup |
| FIPS 197 | AES | Geplant (CosmoFS Encryption) |
| Intel SHA Extensions | SHA-NI (sha256rnds2, sha256msg1/2) | Geplant (CPUID Check) |

Entscheidung: SHA-256 als universelle Hash-Funktion. Integer-Arithmetik, kein FP.
RT-Core Hash-Engine: SSE-Register frei (kein User-FPU auf Core 0).

---

## 6. Security Hardening

### 6.1 Address Space Layout Randomization (ASLR)

| Feature | Entropie | Scope |
|---------|----------|-------|
| Stack-Base | 22 Bit | Jeder Prozess |
| mmap-Base | 28 Bit | Jeder Prozess |
| PIE-Base (ET_DYN) | 28 Bit | Position-Independent Executables |
| Heap (brk) | 13 Bit | Jeder Prozess |
| KASLR | 9 Bit | Kernel-Base bei Boot |

Quelle: getrandom() / RDRAND.

### 6.2 W^X (Write XOR Execute)

Keine Page gleichzeitig Writable und Executable.
- mmap(PROT_WRITE|PROT_EXEC) = -EINVAL (oder: WRITE zuerst, dann mprotect zu EXEC)
- JIT (V8): alloc RW → write Code → mprotect RX → execute. Nie gleichzeitig WX.
- Kernel-Code: Read+Execute. Kernel-Daten: Read+Write. Nie beides.

### 6.3 Stack Guard Pages

Unmapped Page am Ende jeder Stack-VMA.
Stack-Overflow → #PF auf Guard Page → SIGSEGV. Keine Silent Corruption.

### 6.4 Syscall Hardening

- fault_recover fuer alle Kernel-Mode Exceptions (nicht nur #PF)
- sigreturn: MXCSR sanitized, Handler-Adresse validiert
- Range-Checks auf alle mlock/mprotect/madvise Ranges
- copy_from_user/copy_to_user: immer user_ok() Pruefung

### 6.5 Spectre/Meltdown

| Mitigation | Spec | CosmoRT Scope |
|-----------|------|---------------|
| KPTI (Kernel Page Table Isolation) | — | Separate User/Kernel Page-Tables (geplant) |
| Retpoline | — | Indirect Branch Mitigation (-mindirect-branch=thunk) |
| IBRS/IBPB | Intel SDM | Indirect Branch Prediction Barrier |
| SSBD | Intel SDM | Speculative Store Bypass Disable |

Entscheidung: Minimum Retpoline + IBPB bei Kontextwechsel. KPTI geplant.

---

## 7. Moderne Syscalls

Zusaetzlich zu den Linux-ABI Basics (Sektion 3):

| Syscall | Referenz | CosmoRT Scope |
|---------|----------|---------------|
| io_uring_setup/enter/register | man 2 io_uring_setup | Async I/O (libuv/Node.js 22+) |
| memfd_create | man 2 memfd_create | Anonymer fd (Chrome, Wayland, SharedArrayBuffer) |
| copy_file_range | man 2 copy_file_range | Kernel-seitige Dateikopie (cp, rsync) |
| close_range | man 2 close_range | Bulk fd-Close (exec Cleanup, glibc 2.34+) |
| pidfd_open | man 2 pidfd_open | Race-freies Process-Management |
| pidfd_send_signal | man 2 pidfd_send_signal | Signal via pidfd (kein PID-Reuse Race) |

Entscheidung: io_uring ist Pflicht (Node.js Performance). memfd_create ist Pflicht (Chrome/Wayland).

---

## 8. Observability

| Feature | Referenz | CosmoRT Scope |
|---------|----------|---------------|
| /proc/pid/maps | Linux procfs | Memory-Mappings pro Prozess (gdb, strace) |
| /proc/pid/status | Linux procfs | Prozess-Status (Name, State, VmRSS) |
| /proc/pid/cmdline | Linux procfs | Kommandozeile (ps, htop) |
| /proc/meminfo | Linux procfs | Systemweiter Speicher-Status |
| /proc/ksm | CosmoRT | KSM Dedup Statistik |
| perf_event_open | man 2 perf_event_open | Hardware Performance Counters (geplant) |

---

## 9. Hardware / Treiber

Kernel: nur 5 Primitives (MMIO, DMA, IRQ, PCI, FW) via cosmort.h.
Alle Geraetetreiber in Userspace oder als Kernel-Module ueber cosmort.h.

### 9.1 PCI

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| PCI Local Bus Spec 3.0 | PCI-SIG | Config Space (Port I/O 0xCF8/0xCFC) |
| PCIe Base Spec 5.0 | PCI-SIG | MMIO Config, MSI-X (geplant) |

### 9.2 VirtIO

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| VirtIO Spec 1.2 | https://docs.oasis-open.org/virtio/ | virtio-net, virtio-blk, virtio-gpu, virtio-input |
| VirtIO PCI Transport | VirtIO Spec §4.1 | Capability Structure, Notify, ISR |

### 9.3 E1000

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Intel 82540EM (E1000) | Intel Datasheet | TX/RX Descriptors, MMIO Registers |

### 9.4 Hyper-V

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Hyper-V TLFS 6.0b | Microsoft Top-Level Functional Spec | Hypercall Page, SynIC, VMBus, MSRs |
| VMBus Protocol | TLFS Chapter 11 | Channel Offer/Open/Close, GPA Direct |

### 9.5 Serial

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| 16550 UART | National Semiconductor | COM1 (0x3F8), Polling TX/RX |

### 9.6 Userspace-Treiber (via cosmort.h)

| Spec | Referenz | Treiber |
|------|----------|---------|
| NVMe Spec 2.0 | NVM Express | Moderne SSDs, Submission/Completion Queues |
| xHCI Spec 1.2 | USB-IF | USB 3.x Host Controller, Device Enumeration |
| Intel HD Audio Spec | Intel | Audio Codec, Streams, DMA Buffer Descriptor List |
| AHCI Spec 1.3.1 | Intel | SATA Controller (Legacy-SSDs/HDDs) |

Entscheidung: Alle Geraetetreiber ausser NIC/CosmoFS in Userspace via cosmort.h.

---

## 10. Terminal / VT

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| ECMA-48 | Control Functions for Coded Character Sets | CSI Sequences, SGR (Colors) |
| VT100/VT220 | DEC Terminal Reference | Cursor Movement, Scrolling, Erase |
| XTerm Control Sequences | https://invisible-island.net/xterm/ | Alternate Screen (?1049h/l, geplant) |
| Unicode 15.0 | https://unicode.org | UTF-8 Decoding, Glyph Mapping |
| Linux Input Events | include/uapi/linux/input-event-codes.h | KEY_*, REL_*, ABS_* |

Entscheidung: TERM=xterm-256color. ANSI 16-Color Palette (Bernstein-Theme).

---

## 11. Dateisystem

### 11.1 VFS

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| POSIX.1-2017 File System | IEEE 1003.1 | Path Resolution, Symlinks, Permissions |
| Linux procfs | Documentation/filesystems/proc.rst | /proc/pid/*, /proc/meminfo, /proc/ksm |

### 11.2 CosmoFS v2 (geplant)

| Spec | Referenz | CosmoRT Scope |
|------|----------|---------------|
| Merkle Tree / Content-Addressing | — | SHA-256 pro 4KB Block |
| Copy-on-Write B+ Tree | — | Crash-Safe ohne Journal |
| S3 API | https://docs.aws.amazon.com/s3/ | Cloud-Sync Backend |

---

## 12. Architektur-Entscheidungen

Nicht aus einem Standard, sondern CosmoRT-eigen.

| Entscheidung | Begruendung |
|-------------|-------------|
| 1 RT-Core + N Compute-Cores | Deterministische I/O-Latenz, kein Lock zwischen Partitionen |
| ARINC 653 Partitionierung | RT-Core formal bounded, Compute best-effort |
| SPSC Channels (kein Shared Lock) | Lock-free, kein Priority Inversion, WCET berechenbar |
| Alle Allokationen O(1) | Slab, Free-List, Bitmap — kein O(n) Scan |
| Alle Lookups O(1) oder O(log n) | Hash-Tabellen, AVL-Tree — kein linearer Scan |
| SSE2 Maximum (kein AVX) | WASM SIMD Limit, FXSAVE reicht |
| Single-User | Kein UID/GID Enforcement, Permissions gespeichert aber nur +x enforced |
| Kernel-DNS/DHCP nur Boot | Anwendungsprotokolle gehoeren in Userspace |
| CosmoFS einziger Kernel-FS | Externe FS (FAT32, ext4, NTFS) als Userspace-Daemons |
| SHA-256 als universeller Hash | KSM, CosmoFS Dedup, Cloud-Sync — eine Engine, drei Konsumenten |
| CUBIC statt Reno | Modern, RFC 8312, bessere Performance bei hoher Bandbreite |
| Kein Swap | MADV_FREE + KSM-Dedup + THP statt Disk-Swap |

---

## 13. Userland: Alpine Linux + musl

| Komponente | Quelle | Bemerkung |
|------------|--------|-----------|
| libc | musl 1.2.x | Statisch + dynamisch, ld-musl-x86_64.so.1 |
| Paketmanager | apk-tools (Alpine) | ~15.000 Pakete, musl-nativ |
| Coreutils | busybox (Alpine) | Ein Binary, ~300 Applets |
| Shell | bash oder busybox sh | Alpine-Paket |
| Compiler | gcc (Alpine) | musl-basiert, cross-compile-faehig |
| Node.js | Alpine nodejs Paket | Self-Hosting: Claude Code |
| Init | Eigener Init oder OpenRC | Kein systemd |

Entscheidung: Kein eigenes Userland. Alpine liefert alles oberhalb der Syscall-Schicht.
CosmoRT implementiert die Linux-Syscall-ABI, Alpine-Binaries laufen unveraendert.

---

## 14. Abweichungen von Linux

Bewusste Abweichungen. Dokumentiert damit sie nicht als Bug behandelt werden.

| Feature | Linux | CosmoRT | Grund |
|---------|-------|---------|-------|
| IRQ-Verteilung | RSS/RPS/RFS | Alle auf Core 0 | RT-Core Modell |
| Scheduler | CFS (fair) | EDF + Priority | Realtime-faehig |
| Page-Reclaim | kswapd + Direct Reclaim | MADV_FREE Lazy + KSM | Kein Swap-Device |
| Netfilter | iptables/nftables | Keiner | Single-User, kein Firewall im Kernel |
| Namespaces | cgroups + namespaces | Keine | Single-User |
| Security Modules | SELinux/AppArmor | Keiner | Single-User |
| SysV IPC | shmget/semget/msgget | Nicht implementiert | Pipes/Unix-Sockets/Futex reichen |
| Filesystem Journaling | ext4 Journal | CosmoFS COW (kein Journal) | COW = crash-safe by design |
| Dynamic Linker | ld-linux.so | ld-musl-x86_64.so.1 | musl libc (statisch + dynamisch) |
