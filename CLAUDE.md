# CosmoRT

Linux-ABI-kompatibler Realtime-Microkernel. Kein Linux-Kernel.
C11, x86_64 (ARM64 geplant). UEFI Boot, Single-User.

## Headers

```
include/public/
  cosmo_rt.h   Treiber-API: 5 HW-Primitives, NIC/Block Registration.
  cosmo_ui.h   CosmoUI-API: Display, Audio, Input, Power (Stubs).

include/internal/
  linux.h      Linux x86_64 ABI (Syscalls, Structs, Errno). Nur Kernel + libc.
  process.h, sched.h, vma.h, ... (alle Kernel-Interna)
```

Sichtbarkeit:
- Kernel-Code: -Iinclude/public -Iinclude/internal (beides)
- Treiber: -Iinclude/public (kein internal!)
- Tests (ktest): -Iinclude/public -Iinclude/internal (braucht linux.h)
- Userspace: -Iinclude/public

Kein Consumer braucht mehr als eine Datei:
- CosmoPX libc: linux.h → uebersetzt POSIX in Syscalls
- Treiber: cosmo_rt.h → spricht Hardware
- CosmoUI: cosmo_ui.h → spricht Kernel-Subsysteme
- Programme: sehen nur was libc ihnen gibt, nie Header direkt

Linux x86_64 ELF-Binaries (statisch und dynamisch) laufen unveraendert.

## cosmo_rt.h (Treiber)

```
mmio_map, dma_alloc, irq_register, pci_config_read, fw_load
nic_driver_t + net_nic_register
blk_driver_t + blk_register
```

## cosmo_ui.h (CosmoUI)

```
Display    surface_create/present/destroy (VSync, Multi-Monitor)
Audio      device_open/submit/capture/close (Multi-Channel, Multi-Device)
Input      device_read (KEY_*, REL_*, ABS_* — RT-Core, <1ms)
Power      state/suspend/shutdown/reboot (ACPI)
```

USB-Protokoll, Kamera/UVC, Drucker, Bluetooth, WLAN = Userspace.
USB-Devices durch Klasse geroutet: HID → Input, Audio → Audio, Storage → Block.

## Filesysteme

```
Kernel:     CosmoFS (Root, Performance-kritisch), ramfs, procfs
Userspace:  FAT32, ext4, NTFS, NFS, SMB (Latenz-tolerant, ueber Block-I/O)
```

CosmoFS ist der einzige FS-Treiber im Kernel. Externe Medien (USB, Netzwerk)
werden von Userspace-Daemons gemountet die Block-I/O ueber cosmo_rt.h sprechen.

## Core-Modell: RT + Compute (SMP 2+)

```
RT-Core (Core 0):           Compute-Cores (Core 1..N):
  Alle IRQs                   Keine IRQs
  Audio, VSync, Input         Userspace-Prozesse
  Netzwerk RX/TX              fork/exec/mmap
  DMA-Completion              Compiler, Runtime, ...
```

RT-Core = I/O-Prozessor. Deterministisch, <2% Last, mlockall.
Compute-Cores = ungestoerter Userspace. Keine IRQs, kein I/O-Polling.
Kommunikation nur ueber Lock-free Ringbuffer. RT ↔ Compute: nie.

| Cores | RT | Compute |
|-------|-----|---------|
| 2     | 1   | 1       |
| 4     | 1   | 3       |
| 8+    | 1   | 7+      |

## Stack

```
CosmoUI  ~/Git/CosmoUI    UI, WASM-Runtime, Protokoll-Stacks
CosmoPX  ~/Git/CosmoPX     libc, Userland, Node.js, Claude Code
CosmoJS  ~/Git/CosmoJS     JS-Engine
CosmoRT  ~/Git/CosmoRT     Kernel (dieses Repo)
```

## Verzeichnisse

```
include/
  public/        cosmo_rt.h (Treiber-API), cosmo_ui.h (CosmoUI-API)
  internal/      linux.h (ABI), process.h, sched.h, vma.h, ... (Kernel-Interna)
src/kernel/
  core/          main, irq, sched, timer, smp, tss, percpu
  mm/            page_alloc, paging, vma, slab, random
  proc/          process, elf
  syscall/       dispatch, sys_{file,mem,proc,signal,time,ipc}
  ipc/           futex, pipe, net_port
  fs/            vfs, cosmofs, procfs
  net/           TCP/IP, socket, unix_socket
  event/         epoll, eventfd, timerfd, inotify
  vt/            VT, pty, framebuffer, input
  hw/            kexec, serial, hyperv
src/drivers/     NUR include/public/ (kein Zugriff auf Kernel-Interna)
src/boot/        UEFI Bootloader
src/user/        init, ktest, ld-cosmo.so, e1000d, svcmgr
test/            unit/ + crash/ (Self-Registering: TEST/CRASH_TEST Macros)
tools/           mkfs, cosmo_cp, mkimage.sh, mkfont.py
```

## Build

```sh
make                # Kernel → build/BOOTX64.EFI
make test-hw        # ktest-Suite in QEMU
make qemu-gui       # VT mit Bernstein-Palette
make qemu-disk      # Boot von disk.img (GPT: ESP + CosmoFS)
make vhdx           # Hyper-V Image
```

## Regeln

Build:
- Warnings = Errors (-Werror)
- make test-hw muss gruen sein
- Freestanding C11, GNU as fuer Assembly (.S, kein NASM)

ABI:
- linux.h: exakt Linux x86_64 ABI, keine Abweichungen
- Jedes Define in linux.h muss im Kernel implementiert UND getestet sein
- Keine Magic Numbers — benannte Konstanten aus linux.h
- Keine Stubs (return 0) — korrekt implementieren oder -ENOSYS

Code-Organisation:
- Bottom-Up: Helpers oben, Caller unten. Keine Forward-Declarations
- Hot-Path frei von Strings und Error-Output (cold-Funktionen auslagern)
- __attribute__((hot)) auf Syscall-Dispatch, __attribute__((cold)) auf Panic/Init
- __builtin_expect auf Fehlerpfade im Hot-Path
- Legacy-Syscalls delegieren an moderne *at-Varianten (Einzeiler-Wrapper)

Portabilitaet:
- src/kernel/ hat kein inline-asm — alles ueber arch_*() in src/arch/
- src/arch/{x86_64,aarch64,riscv64}/ — pro Architektur
- Treiber: nur include/public/, nie Kernel-Interna

Tests:
- Jeder neue Syscall/Fix bekommt Tests
- TEST() fuer Unit-Tests, CRASH_TEST() fuer Adversarial-Tests
- Self-Registering via Linker-Section — kein main.c anfassen
