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

## Userspace-Treiber (Microkernel)

Alle Treiber laufen im Userspace, nicht im Kernel. Kommunikation mit
dem Kernel via IPC (Interrupt-Delivery, DMA-Setup, MMIO-Mapping).

- Filesystem
- Netzwerk-Stack (TCP/IP)
- GPU-Treiber
- Audio-Treiber
- Input-Treiber
- USB, WiFi, Bluetooth, Storage

### Linux-Treiber-Transpiler

Linux-Treiber werden per LLM-Transpiler nach CosmoRT uebersetzt.
Die Uebersetzung ist mechanisch — Linux Driver API → POSIX Userspace:

| Linux Kernel API          | CosmoRT Userspace             |
|---------------------------|-------------------------------|
| `pci_ioremap_bar()`       | `mmap(/dev/pci/...)`          |
| `readl/writel`            | Direkter Speicherzugriff      |
| `request_irq()`           | IPC-Port fuer Interrupt-Msgs  |
| `kmalloc/kfree`           | `malloc/free`                 |
| `dma_alloc_coherent`      | `mmap` mit DMA-Flag           |
| `spinlock_t`              | `pthread_mutex`               |
| `module_init/exit`        | `main()` + Init/Cleanup       |
| `probe/remove`            | Init/Cleanup Funktionen       |
| Interrupt-Context/Softirq | Entfaellt (alles Userspace)   |

Ergebnis: sauberere Treiber. Kernel-Komplexitaet (IRQ-Context, RCU,
Softirqs, Workqueues) entfaellt im Microkernel. Der transpilierte
Treiber ist simpler als das Linux-Original.

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
