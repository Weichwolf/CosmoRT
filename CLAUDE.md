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

## Ausgangsbasis: llmos

~/Git/llmos/ ist ein funktionierender Microkernel (22K Zeilen C):
- UEFI Boot, GDT, IDT, Page Tables
- EDF-Scheduler (Tickless, Bandwidth-Server)
- Capability-basiertes IPC (seL4-Stil)
- Paging, SMP, APIC Timer
- Treiber: xHCI USB, E1000 Netzwerk, virtio Block
- Grafik, Browser, JS-Engine (alles Ring 0)

CosmoRT baut darauf auf. Was sich aendert:
1. POSIX-Syscall-Layer drauflegen (fork, exec, open, read, write, ...)
2. Treiber aus Ring 0 in Userspace verschieben
3. Eingebauten Browser/Desktop entfernen (wird CosmoUI/CosmoJS)
4. CosmoPX libc als Userspace-Fundament

Der Kernel-Core (Boot, Scheduler, IPC, Paging, IRQ) bleibt.

## Ziel

Minimaler Kernel der POSIX-Syscalls implementiert.
Alles was Userspace sein kann ist Userspace (Microkernel).

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

### Drei Schichten

```
Userspace:
  Protokoll-Stacks (WiFi, USB, Audio, Display)
    reine Software, portiert von Linux, testbar ohne Hardware
  Treiber-Adapter (iwl, xhci, nvme, hda, ...)
    duenn, Hardware-spezifisch, MMIO via gemappten Speicher

Kernel (CosmoRT):
  5 Hardware-Primitives (einmalig bei Init aufgerufen)
  Scheduler, Memory, IPC
```

### Kernel: 5 Hardware-Primitives

```c
int cosmo_mmio_map(uint64_t phys, size_t len, void **virt);
int cosmo_dma_alloc(size_t len, void **virt, uint64_t *phys);
int cosmo_irq_register(int irq, void (*handler)(void *), void *ctx);
int cosmo_pci_config_read(int bus, int dev, int fn, int reg, uint32_t *val);
int cosmo_fw_load(const char *name, void **data, size_t *len);
```

Das ist die GESAMTE Hardware-Abstraktion. Kernel macht das Mapping,
Userspace macht den Zugriff. Kein Syscall pro Register-Zugriff.

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

## Regeln

- `make test` muss immer gruen sein.
- Warnings = Errors.
- C11. Assembly nur in arch/ (Boot, Context-Switch, Syscall-Entry).
- Kernel kommt zuletzt. Erst CosmoPX (libc), getestet auf Linux.
