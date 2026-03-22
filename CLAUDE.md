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

## Design-Entscheidungen

- Single-User: kein root/sudo, Capabilities statt Permissions
- Linux Syscall-Nummern (x86_64 ABI)
- RT/POSIX Koexistenz: 2 Cores → Preemption, 4+ → Core-Isolation
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
