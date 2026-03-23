# CosmoRT

Linux-ABI-kompatibler Realtime-Microkernel. Kein Linux-Kernel.
C11, x86_64 (ARM64 geplant). UEFI Boot, Single-User.

## Public Headers (include/public/)

```
linux.h      Exakt Linux x86_64 ABI. Nur fuer CosmoPX libc.
cosmo_rt.h   Treiber-API: 5 HW-Primitives, NIC/Block Registration.
cosmo_ui.h   CosmoUI-API: Display, Audio, Input, Power.
```

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
  public/        linux.h (ABI), cosmo.h (Treiber-API)
  internal/      process.h, sched.h, vma.h, ... (Kernel-Interna)
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

- Warnings = Errors (-Werror)
- make test-hw muss gruen sein
- Freestanding C11, minimales x86 Assembly
- Treiber: nur include/public/, nie Kernel-Interna
- linux.h: exakt Linux ABI, keine Abweichungen
