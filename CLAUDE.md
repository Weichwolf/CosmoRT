# CosmoRT

Realtime-Microkernel fuer CosmoOS. POSIX-kompatibel, Single-User.
C11, x86/ARM/RISC-V. Mindesthardware: Intel N4020 (2 Cores).

## Stack

```
CosmoPX (libc)    ~/Git/CosmoPX
  |
CosmoRT (Kernel)  dieses Repo
  |
Hardware          x86/ARM/RISC-V
```

## Ausgangslage

~/Git/llmos/ hat einen funktionierenden Microkernel (22K Zeilen):
- UEFI Boot, GDT, IDT, Page Tables, APIC Timer
- EDF-Scheduler (Tickless, Bandwidth-Server, SMP)
- Capability-basiertes IPC (seL4-Stil, synchron + async)
- Paging, TSS, IRQ-Handling
- Treiber: xHCI USB, E1000 Netzwerk, virtio Block
- Browser, JS-Engine, Desktop (alles Ring 0)

~/Git/CosmoPX/libc/ hat eine eigenstaendige POSIX libc (737 Symbole):
- Statische Binaries ohne glibc funktionieren
- printf, malloc, pthreads, math, stat, time, errno — alles da
- 141 musl libc-tests gruen

~/Git/CosmoLib/ hat HTTP/TLS/Crypto/JSON/RegExp/etc.

## Ziel

CosmoRT = llmos Kernel-Core + POSIX-Syscall-Layer.
Bootet in QEMU. Fuehrt statische CosmoPX-Binaries aus.

Validierung: `/tmp/cosmo-sysroot/usr/bin/fetch https://example.com/`
laeuft auf CosmoRT in QEMU und gibt HTML aus.

## RT-Modell

RT und POSIX koexistieren. Kein Kompromiss — beide bekommen was sie brauchen.

**Skalierung nach Core-Anzahl:**

| Cores | Strategie |
|-------|-----------|
| 2 (N4020) | SCHED_FIFO RT-Threads preemten POSIX-Threads |
| 4 (Laptop) | 1 RT-Core isoliert + 3 POSIX-Cores |
| 8+ (Desktop) | 2 RT-Cores isoliert + N POSIX-Cores |

Selber Userspace-Code, selbe Kommunikation (cl_ring). Scheduling-Strategie
ist Kernel-Konfiguration, transparent fuer Anwendungen.

**2-Core Modus (N4020):**
- RT-Thread: SCHED_FIFO, hoechste Prioritaet, preemptiv
- Audio-Callback: ~0.1ms alle 10ms = 1% CPU
- Kein Core-Pinning, kein mlockall noetig
- Soft-RT reicht — Zeitbudgets sind gross genug

**4+ Core Modus:**
- RT-Cores: isolcpus-Aequivalent, kein POSIX-Scheduling
- mlockall auf RT-Cores (kein Page-Fault)
- Kommunikation RT<->POSIX nur ueber Lock-free Ringbuffer (cl_ring)
- Keine Locks kreuzen die RT/POSIX-Grenze

**RT-Anwendungsfaelle:**
- Audio-Callback (48kHz, 10ms Buffer, ~0.1ms Arbeit)
- Display Present (VSync, 60/120Hz)
- Input-Polling (Keyboard, Mouse, Touch — Latenz <1ms)
- Video-Frame-Decode (Deadline pro Frame: 16ms bei 60fps)

## Kernel-Verantwortung

- Scheduling: Fixed-Priority mit Priority-Inheritance (IEEE 1003.1b)
- CPU-Affinity: Thread-to-Core Pinning (sched_setaffinity)
- Memory: Virtual Memory, Copy-on-Write, Shared Memory, mmap, mlockall
- IPC: Synchrone Messages (L4-Stil) + Async Ports
- Syscalls: POSIX-kompatibel. Linux Syscall-Nummern wo moeglich.

## Treiber-Architektur

### Verzeichnisstruktur

```
src/kernel/       Kernel-Core (Scheduler, VM, Syscalls, IPC)
                  Importiert: nichts aus src/drivers/
src/kernel/hw.h   5 Hardware-Primitives (die einzige HW-Schnittstelle)
src/kernel/net.h  Netzwerk-Stack (NIC-agnostisch via nic_driver_t)

src/drivers/      Treiber-Adapter (Hardware-spezifisch)
                  Importiert NUR: hw.h, config.h, serial.h, net.h
                  Importiert NICHT: process.h, sched.h, syscall.h, vma.h
                  → Mechanisch in Userspace extrahierbar
src/drivers/net/  NIC-Treiber (e1000, spaeter virtio-net, r8169)
```

Regel: Treiber in src/drivers/ nutzen AUSSCHLIESSLICH die 5 Primitives
aus hw.h + Subsystem-Registrierung (z.B. net_nic_register). Keine
direkten Kernel-Interna. Wenn ein Treiber process.h braucht, ist das
ein Architektur-Fehler.

### NIC-Treiber-Interface

```c
// Jeder NIC-Treiber implementiert dieses Interface:
typedef struct {
    int  (*send)(const void *data, uint16_t len);
    int  (*recv)(void *buf, uint16_t bufsize);
    void (*get_mac)(uint8_t mac[6]);
    const char *name;
} nic_driver_t;

// Treiber registriert sich beim Netzwerk-Stack:
void net_nic_register(const nic_driver_t *nic);
// Netzwerk-Stack nutzt registrierten Treiber fuer send/recv.
// Kein #include "e1000.h" im Stack. Kein switch(driver_type).
```

### 5 Hardware-Primitives (hw.h)

```c
int cosmo_mmio_map(uint64_t phys, size_t len, void **virt);
int cosmo_dma_alloc(size_t len, void **virt, uint64_t *phys);
int cosmo_irq_register(int irq, void (*handler)(void *), void *ctx);
int cosmo_pci_config_read(int bus, int dev, int fn, int reg, uint32_t *val);
int cosmo_fw_load(const char *name, void **data, size_t *len);
```

Das ist die GESAMTE Hardware-Abstraktion. Kernel macht das Mapping,
Treiber machen den Zugriff. Kein Syscall pro Register-Zugriff.
Spaeter werden die 5 Primitives zu Syscalls → Treiber in Userspace.

### Userspace: Protokoll-Stacks (portiert von Linux)

Reine Software, keine Hardware-Abhaengigkeit, testbar in QEMU mit
Mock-Backends. Einmal portieren, fuer alle Treiber wiederverwendbar.

| Stack | Linux-Quelle | Funktion |
|---|---|---|
| Wireless | mac80211 + cfg80211 | WiFi-Protokoll |
| Netzwerk | netdev + TCP/IP | Paket-Verarbeitung |
| USB | USB Core | USB-Protokoll |
| Audio | ALSA Core | Audio-Routing |
| Display | DRM/KMS | Modesetting, Buffering |
| Storage | Block-Layer | I/O Scheduling |

### Userspace: Treiber-Adapter (pro Hardware)

Duenn (~500-2000 Zeilen). Nutzt den Protokoll-Stack + die 5 Kernel-
Primitives. Hardware-spezifischer Code (Register-Adressen, Firmware-
Protokoll) kommt aus dem Linux-Treiber — nur die Zeilen die wirklich
Hardware anfassen, nicht der gesamte 100K-Zeilen Treiber.

10 Adapter decken 90% der Hardware ab:

| Adapter | Hardware | Zeilen |
|---|---|---|
| i915/amdgpu | Intel/AMD GPU | ~2000 |
| iwlwifi | Intel WiFi | ~1000 |
| r8169 | Realtek Ethernet | ~500 |
| hda | Intel HD Audio | ~500 |
| xhci | USB 3.0 | ~1000 |
| nvme | NVMe Storage | ~500 |
| ahci | SATA Storage | ~500 |
| hid | USB Keyboard/Mouse | ~300 |
| ps2 | Legacy Input | ~200 |
| acpi | Power Management | ~500 |

### Entwicklungsplan

**Phase 1: QEMU (keine Hardware noetig)**
- CosmoRT bootet in QEMU
- virtio-Treiber (virtio-net, virtio-gpu, virtio-blk, virtio-input)
- Protokoll-Stacks portieren und gegen virtio-Backends testen

**Phase 2: Protokoll-Stacks (keine Hardware noetig)**
- Linux mac80211/cfg80211 → CosmoOS Wireless-Stack
- Linux USB Core → CosmoOS USB-Stack
- Testbar mit Mock-Treibern in QEMU

**Phase 3: Echte Hardware**
- Treiber-Adapter schreiben (braucht echte Hardware)
- 50 Euro Hardware (RPi + USB-Geraete) fuer Entwicklung
- Record/Replay von Linux Register-Traces fuer Regression-Tests

### Referenz: Wer macht es aehnlich?

| OS | Ansatz | Status |
|---|---|---|
| Fuchsia | Userspace-Treiber via Driver Framework | Production (Nest Hub) |
| Genode/Sculpt | Linux-DDE Compat-Layer | Production |
| Minix 3 | Alle Treiber Userspace, Auto-Restart | Forschung |
| seL4/CAmkES | Formal verifiziert, Userspace-Treiber | Militaer/Aerospace |

Alle mit limitiertem Hardware-Support. Breiter Support braucht
die 10 Adapter + portierte Protokoll-Stacks.

## CosmoRT-spezifische Syscalls

Ueber POSIX hinaus, fuer Multimedia-RT:

- GPU Command-Buffer Submit (Zero-Copy, Fence)
- Audio-Buffer Submit (RT-safe, Zero-Copy)
- Display Present (VSync-synchron)
- IPC Fast-Path (Lock-free Message-Passing)

## Single-User

Kein Multi-User. Kein root/sudo. Kein Permission-System.
Capabilities statt File-Permissions.

## Specs

**POSIX:**
- IEEE 1003.1-2024 POSIX.1 (Syscall-Interface)
- IEEE 1003.1b Realtime Extensions (SCHED_FIFO, SCHED_RR, mlockall,
  clock_gettime, timer_create, mq_open, sem_open, shm_open)
- IEEE 1003.1c Threads (pthread_create, mutex, condvar, rwlock,
  pthread_setschedparam, pthread_setaffinity_np)
- IEEE 1003.1j Advanced Realtime (spawn, typed memory)

**Scheduling (IEEE 1003.1b):**
- sched_setscheduler / sched_getscheduler
- sched_setparam / sched_getparam (RT-Prioritaeten)
- sched_setaffinity / sched_getaffinity (Core-Pinning)
- sched_yield
- SCHED_FIFO: Fixed-Priority, kein Timeslice
- SCHED_RR: Round-Robin innerhalb gleicher Prioritaet
- SCHED_OTHER: Default fuer POSIX-Threads
- Priority-Inheritance auf Mutexes (PTHREAD_PRIO_INHERIT)

**Memory (IEEE 1003.1b):**
- mlockall / munlockall (RT-Cores: kein Page-Fault)
- mmap / munmap / mprotect
- shm_open / shm_unlink (Shared Memory fuer IPC + Surface-Sharing)
- madvise (MADV_DONTNEED fuer Arena-Reset)

**Timer (IEEE 1003.1b):**
- clock_gettime / clock_nanosleep (CLOCK_MONOTONIC)
- timer_create / timer_settime (High-Resolution Timer)
- nanosleep

**IPC (IEEE 1003.1b):**
- mq_open / mq_send / mq_receive (Message Queues)
- sem_open / sem_wait / sem_post (Named Semaphores)
- shm_open (Shared Memory)

## Phase 1: Kernel-Core extrahieren

Kopiere aus ~/Git/llmos/src/ nach ~/Git/CosmoRT/src/:

**Behalten (Kernel-Core):**
- src/boot/boot.c — UEFI Boot, ExitBootServices
- src/kernel/main.c — Kernel-Entry
- src/kernel/paging.c — Page Tables, Virtual Memory
- src/kernel/sched.c + edf.c — Scheduler
- src/kernel/ipc.c — IPC
- src/kernel/irq.c — Interrupt-Handling
- src/kernel/timer.c — APIC Timer
- src/kernel/smp.c — Multi-Core
- src/kernel/tss.c — Task State Segment
- src/kernel/sse.c — SSE/AVX State Save
- src/kernel/process.c — Process-Management
- src/kernel/serial.c — Debug-Output (Serial Console)
- src/arch/ — Architektur-spezifischer Code

**NICHT kopieren (wird Userspace):**
- src/browser/ — wird CosmoUI
- src/js/ — wird CosmoJS
- src/gfx/ — wird CosmoLib/CosmoUI
- src/crypto/ — wird CosmoLib
- src/kernel/https.c, http_parse.c, net.c — wird CosmoLib
- src/kernel/agent.c — wird CosmoJS App
- src/kernel/objstore.c — wird Userspace-Service
- src/drivers/ — bleibt erstmal, wird spaeter Userspace

## Phase 2: POSIX-Syscall-Layer

Implementiere einen Syscall-Handler der POSIX-Syscalls entgegennimmt.
Linux Syscall-Nummern verwenden (x86_64 Syscall-Tabelle).

```c
// src/kernel/syscall.c
// Entry via syscall-Instruktion (STAR/LSTAR MSR)

long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    switch (num) {
        case SYS_read:    return sys_read((int)a1, (void*)a2, (size_t)a3);
        case SYS_write:   return sys_write((int)a1, (const void*)a2, (size_t)a3);
        case SYS_open:    return sys_open((const char*)a1, (int)a2, (int)a3);
        case SYS_close:   return sys_close((int)a1);
        case SYS_mmap:    return sys_mmap(a1, a2, a3, a4, a5, a6);
        case SYS_munmap:  return sys_munmap(a1, a2);
        case SYS_brk:     return sys_brk(a1);
        case SYS_exit:    sys_exit((int)a1); return 0;
        case SYS_getpid:  return sys_getpid();
        // ...
        default:          return -ENOSYS;
    }
}
```

**Phase 2a — Minimaler Satz fuer "Hello World":**
- SYS_write (1) — stdout Output
- SYS_exit_group (231) — Prozess beenden
- SYS_brk (12) — Heap (fuer malloc)
- SYS_mmap (9) — Memory Mapping (fuer malloc, Stack)
- SYS_munmap (11) — Memory freigeben

**Phase 2b — Erweiterung fuer fetch:**
- SYS_open/close/read/write — File I/O
- SYS_socket/connect/sendto/recvfrom — Netzwerk
- SYS_getaddrinfo — DNS (oder eigener Resolver)
- SYS_clock_gettime — Zeitmessung
- SYS_poll — I/O Multiplexing
- SYS_clone/wait4 — Prozesse (fuer pthreads)
- SYS_futex — Synchronisation (fuer pthread_mutex)

## Phase 3: ELF-Loader

Lade statische ELF-Binaries (CosmoPX-kompiliert) in den Userspace:

```c
// src/kernel/elf.c
int load_elf(const void *elf_data, size_t len, uint64_t *entry_point) {
    // Parse ELF Header
    // Fuer jedes PT_LOAD Segment: mmap in Userspace
    // Stack allozieren (8MB, Guard Page)
    // entry_point = e_entry
    // argc/argv/envp auf Stack legen
    return 0;
}
```

Statische ELF-Binaries haben keine Dynamic-Linking-Abhaengigkeiten.
Nur PT_LOAD Segmente in den Adressraum mappen und zum Entry-Point springen.

## Phase 4: QEMU Boot

```makefile
qemu: kernel.efi
    qemu-system-x86_64 \
        -bios /usr/share/ovmf/OVMF.fd \
        -drive format=raw,file=fat:rw:esp \
        -serial stdio \
        -m 256M \
        -smp 2 \
        -device virtio-net-pci \
        -device virtio-blk-pci,drive=hd0 \
        -drive id=hd0,file=disk.img,format=raw
```

ESP (EFI System Partition) enthaelt:
- EFI/BOOT/BOOTX64.EFI — CosmoRT Kernel
- init — Erste Userspace-Binary (statisch, CosmoPX)

Init-Prozess:
```c
// init.c (kompiliert gegen CosmoPX libc, statisch)
int main(void) {
    write(1, "CosmoOS booted!\n", 16);
    // Spaeter: mount filesystem, start shell
    for(;;) pause();
}
```

## Phase 5: Validierung

1. QEMU bootet CosmoRT
2. Kernel laedt init (statische CosmoPX-Binary)
3. init gibt "CosmoOS booted!" auf Serial Console aus
4. Spaeter: Shell, fetch, Ruby, Homebrew

## Dateisystem

Fuer Phase 4-5: Initrd oder eingebettetes CPIO-Archiv.
Kein richtiges Filesystem noetig fuer den ersten Boot.

Spaeter: virtio-blk + einfaches Filesystem (ext2-read oder eigenes).

## Treiber (erstmal im Kernel)

Fuer Phase 4: virtio-Treiber direkt im Kernel (einfach, QEMU-kompatibel):
- virtio-console — Serial/Console Output
- virtio-blk — Block-Device fuer Filesystem
- virtio-net — Netzwerk (fuer fetch)

Diese werden SPAETER in den Userspace verschoben (Microkernel-Ziel).
Erstmal funktional, dann sauber.

## Meilensteine

1. Kernel bootet in QEMU, gibt Text auf Serial aus
2. ELF-Loader laedt statische Binary
3. "Hello World" Binary (write + exit Syscalls) laeuft
4. malloc funktioniert (brk/mmap Syscalls)
5. CosmoPX printf-Binary laeuft
6. Netzwerk-Syscalls (socket/connect/read/write)
7. fetch laeuft und gibt HTML aus

## Regeln

- Kernel in C11 + minimales Assembly (arch/)
- llmos-Code als Referenz, nicht blind kopieren — verstehen und anpassen
- QEMU ist die primaere Test-Plattform
- Serial Console fuer Debug-Output (kein Grafik noetig)
- Linux Syscall-Nummern (Kompatibilitaet)
- `make qemu` muss den Kernel booten
- `make test` muss immer gruen sein
- Warnings = Errors
