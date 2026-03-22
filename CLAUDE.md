# CosmoRT

Realtime-Microkernel fuer CosmoOS. POSIX-kompatibel, Single-User.
C11, x86_64 (ARM64 geplant). UEFI Boot, kein libc (Freestanding).

## Stack

```
CosmoUI  ~/Git/CosmoUI    UI + Protokoll-Stacks (Userspace-Treiber)
CosmoPX  ~/Git/CosmoPX     libc + Userland + Node.js + Claude Code
CosmoRT  ~/Git/CosmoRT     Kernel (dieses Repo)
```

## Build & Test

```sh
make                    # → build/BOOTX64.EFI
make qemu               # ktest in QEMU (61 PASS)
make qemu-gui            # VT mit Bernstein-Palette (virtio-keyboard)
make qemu-disk           # Boot von CosmoFS disk.img
make test-hw             # ktest-Suite
```

## Architektur

```
src/kernel/
  core/    main, irq, sched, edf, timer, smp, tss, percpu
  mm/      page_alloc (buddy), paging, vma (AVL), slab, random
  proc/    process, elf (streaming CosmoFS loader)
  syscall/ dispatch + sys_{file,mem,proc,signal,time,ipc} + internal.h
  ipc/     L4-sync, futex (PI), pipe, net_port (ring IPC)
  fs/      vfs, cosmofs (B+ tree, journal, bcache), procfs
  net/     TCP/IP, per-queue locks, socket
  event/   epoll, eventfd, timerfd, signalfd, inotify
  vt/      VT + ANSI, pty, framebuffer, keyboard input
  hw/      5 HW-Primitives, kexec, serial, hyperv
  include/ alle Header (flat)
  gen/     generierte *_bin.h (gitignored)
src/drivers/
  net/     e1000 (in-kernel, soll raus → e1000d userspace)
  blk/     virtio-blk
  gpu/     virtio-gpu
  input/   virtio-input
  hyperv/  vmbus, storvsc, netvsc, hyperv_fb, hv_kbd/mouse/utils
  virtio/  gemeinsamer Transport
src/boot/  UEFI Bootloader
src/user/  init, ktest, ld-cosmo.so, e1000d, svcmgr
tools/     mkfs, cosmo_cp, mkfont.py
```

## Treiber-Regel

src/drivers/ importiert NUR: hw.h, config.h, serial.h, net.h.
NICHT: process.h, sched.h, syscall.h, vma.h.
Wenn ein Treiber Kernel-Interna braucht → Architektur-Fehler.

## Core-Modell: RT + Compute (SMP 2+ only, kein SMP 1)

Mindesthardware: 2 Cores. Kein Single-Core-Support.

```
RT-Core (Core 0):           Compute-Cores (Core 1..N):
  Alle IRQs                   Keine IRQs
  HW-Primitives (hw.h)       Syscalls, Scheduling, VMA, VFS
  Audio-Callback              Userspace-Prozesse
  VSync / Framebuffer-Flip    Node.js, Bash, Doom, Claude
  Input-Polling               fork/exec/mmap/mprotect
  Netzwerk RX/TX              NN-Training, Compiler, ...
  DMA-Completion
```

RT-Core = I/O-Prozessor. Deterministisch, <2% Last, mlockall,
kein Page-Fault, kein POSIX-Scheduling. Kommunikation mit
Compute nur ueber Lock-free Ringbuffer.

Compute-Cores = reine Rechenleistung. Keine IRQs, kein
Timer-Interrupt, kein I/O-Polling. Ungestoerter Userspace
bis der Prozess freiwillig einen Syscall macht. Wie eine
Gaming-Console.

TLB-Shootdown-IPI nur zwischen Compute-Cores. RT-Core
wird nie gestoert (eigener Adressraum, locked Pages).

Scheduling auf Compute-Cores:
- Thread-Placement bei clone: Round-Robin ueber Compute-Cores
- Thread-Migration: erlaubt (trivial wenn TLB-Shootdown funktioniert)
- Rebalancing: 1x/s, laengste Queue → kuerzeste Queue
- RT ↔ Compute: nie

| Cores | RT | Compute |
|-------|-----|---------|
| 2     | 1   | 1       |
| 4     | 1   | 3       |
| 8+    | 1   | 7+      |

## Design-Entscheidungen

- Single-User: kein root/sudo, Capabilities statt Permissions
- Linux Syscall-Nummern (x86_64 ABI)
- SMP 2+: 1 RT-Core (I/O) + N Compute-Cores (Userspace)
- Treiber im Kernel sind temporaer (QEMU-Entwicklung), Ziel: alles Userspace
- $HOME=/home, kein /usr, kein /var

## Meilensteine

1-6: erledigt (boot, ELF, hello, malloc, printf, netzwerk)
7: [ ] CosmoOS bootet in Bash (disk.img mit CosmoPX)
8: [ ] Hyper-V Boot
9: [ ] Claude Code laeuft auf CosmoOS (BOOTSTRAP.md)

## Regeln

- Warnings = Errors (-Werror)
- `make qemu` muss booten, `make test-hw` muss gruen sein
- Freestanding C11 + minimales x86 Assembly
- Serial Console fuer Debug
