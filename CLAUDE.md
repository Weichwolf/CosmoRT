# CosmoRT

Linux-ABI-kompatibler Realtime-Microkernel mit ARINC 653 Partitionierung.

## Headers

```
include/public/
  cosmort.h   Treiber-API: 5 HW-Primitives, NIC/Block Registration.
  cosmoui.h   CosmoUI-API: Display, Audio, Input, Power (Stubs).

include/kernel/
  core/       sched.h, edf.h, percpu.h, smp.h, timer.h, irq.h, rt.h
  mm/         vma.h, paging.h, page_alloc.h, slab.h
  proc/       process.h, thread.h, elf.h
  sys/        syscall.h
  ipc/        futex.h, ipc.h
  fs/         vfs.h, ext2.h, procfs.h, bcache.h
  net/        net.h, socket.h, unix_socket.h, tcp.h, udp.h, arp.h, ip.h, dns.h, dhcp.h, net_port.h, net_util.h
  event/      epoll.h, fd.h
  vt/         vt.h, pty.h, fb.h, input.h
  hw/         serial.h, kexec.h, hyperv.h
  arch/       arch.h, x86_64.h
  config.h, spinlock.h, uaccess.h, memops.h, random.h, ring.h, boot_info.h

include/linux/
  abi.h        Linux x86_64 ABI Master-Include
  syscall.h, errno.h, types.h, ... (ABI-Konstanten)
```

Includes verwenden Subsystem-Pfade: `#include "proc/process.h"`, `#include "core/sched.h"`.

Sichtbarkeit:
- Kernel-Code: -Iinclude/public -Iinclude/kernel -Iinclude (alles)
- Treiber: -Iinclude/public (kein kernel!)
- Tests (ktest): -Iinclude/public -Iinclude/kernel -Iinclude -Itest
- Userspace: -Iinclude/public

Kein Consumer braucht mehr als eine Datei:
- Alpine Userland: musl libc, ld-musl-x86_64.so.1, apk Pakete
- Treiber: cosmort.h → spricht Hardware
- CosmoUI: cosmoui.h → spricht Kernel-Subsysteme
- Programme: sehen nur was musl libc ihnen gibt, nie Kernel-Header direkt

Linux x86_64 ELF-Binaries (statisch und dynamisch) laufen unveraendert.

## cosmort.h (Treiber)

```
mmio_map, dma_alloc, irq_register, pci_config_read, fw_load
nic_driver_t + net_nic_register
blk_driver_t + blk_register
```

## cosmoui.h (CosmoUI)

```
Display    surface_create/present/destroy (VSync, Multi-Monitor)
Audio      device_open/submit/capture/close (Multi-Channel, Multi-Device)
Input      device_read (KEY_*, REL_*, ABS_* — RT-Core, <1ms)
Power      state/suspend/shutdown/reboot (ACPI)
```

UI-Stack: SDL3 laeuft unmodifiziert ueber Standard Linux Device Nodes.
Kernel exposed /dev/fb0 (Framebuffer), /dev/input/event0 (evdev),
/dev/snd/ (ALSA). SDL3's existierende Linux-Backends (fbdev, evdev, ALSA)
funktionieren direkt. Kein SDL3-Fork, kein Custom-Backend, kein X11/Wayland.
App → SDL3 → Linux Device Nodes → cosmoui.h intern → Hardware.
GPU-Beschleunigung optional ueber /dev/dri/ (KMS/DRM, virtio-gpu).

USB-Protokoll, Kamera/UVC, Drucker, Bluetooth, WLAN = Userspace.
USB-Devices durch Klasse geroutet: HID → Input, Audio → Audio, Storage → Block.

## Filesysteme

```
Kernel:     ext2 (Root, read-write), ramfs, procfs
Userspace:  FAT32, ext4, NTFS, NFS, SMB (Latenz-tolerant, ueber Block-I/O)
```

ext2 ist der Root-FS-Treiber im Kernel. Externe Medien (USB, Netzwerk)
werden von Userspace-Daemons gemountet die Block-I/O ueber cosmort.h sprechen.

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

Architektur-Ziel: ARINC 653 Partitionierung.
RT-Core (Partition A): Formal bounded WCET. Kein Shared Lock mit Compute,
kein Dynamic Alloc zur Laufzeit, kein IPI-Wait. Beweisbar deterministisch.
Compute-Cores (Partition B): Best-Effort Desktop. COW, Demand-Paging, Slab.
Keine Einschraenkung, volle Performance.
Partition-Grenze = SPSC Lock-free Channels. Kein Code-Pfad kreuzt die Grenze.

| Cores | RT | Compute |
|-------|-----|---------|
| 2     | 1   | 1       |
| 4     | 1   | 3       |
| 8+    | 1   | 7+      |

## Stack

```
CosmoUI  ~/Git/CosmoUI    UI, WASM-Runtime, Protokoll-Stacks
CosmoJS  ~/Git/CosmoJS     JS-Engine
CosmoRT  ~/Git/CosmoRT     Kernel (dieses Repo)
Alpine   Userland          musl libc, apk, busybox, gcc, Node.js, Claude Code
```

Userland basiert auf Alpine Linux (musl-nativ). Pakete per apk-tools.
Kein systemd — OpenRC oder eigener Init. Kernel + Alpine = komplettes System.

## Verzeichnisse

```
include/
  public/        cosmort.h (Treiber-API), cosmoui.h (CosmoUI-API)
  kernel/        Kernel-Interna (Subsystem-Spiegel: core/, mm/, proc/, net/, ...)
src/kernel/
  core/          main, irq, sched, timer, smp, tss, percpu
  mm/            page_alloc, paging, vma, slab, random
  proc/          process, elf
  sys/           dispatch, sys_{file,fs,mem,proc,sched,signal,time,ipc,net,event,id,cosmo}, stubs
  ipc/           futex, pipe, net_port
  fs/            vfs, ext2, procfs
  net/           TCP/IP, socket, unix_socket
  event/         epoll, eventfd, timerfd, inotify
  vt/            VT, pty, framebuffer, input
  hw/            kexec, serial, hyperv
src/drivers/     NUR include/public/ (kein Zugriff auf Kernel-Interna)
src/boot/        UEFI Bootloader
src/user/        init, crt0.S
test/            unit/ + crash/ (Self-Registering: TEST/CRASH_TEST Macros)
tools/           mkfs, cosmocp, mkimage.sh, mkfont.py
```

## Build

```sh
make                    # Kernel → build/BOOTX64.EFI
make test-hw            # ktest Unit-Tests in QEMU (eigenes ESP, eigenes init)
make test-crash         # Crash/Adversarial Tests
make test-fuzz          # Syscall Fuzzer
make alpine-image       # ext2 Image aus build/alpine-root/
make alpine-test        # TDD: Boot → musl + LTP Tests → Fail → Stop → Poweroff
make qemu-alpine        # Normaler Alpine Boot (OpenRC → getty → login)
make qemu-alpine-gui    # Alpine mit GUI + Keyboard
```

## TDD Workflow

```
1. make alpine-test        → Boot, Tests laufen, stoppt beim ersten Fail
2. Fehler analysieren       → Serial-Log zeigt Test-Name + Output
3. Kernel-Fix implementieren
4. Committen                → jeder Fix ein eigener Commit
5. make alpine-test        → Kernel neu gebaut (wenn Source geaendert),
                              Image neu gebaut (~30s), Boot, Tests laufen
6. Wiederholen bis alle Tests passen
```

make alpine-test haengt von $(EFI_BIN) ab — Kernel wird nur bei Quellcode-
Aenderungen neu kompiliert. mkalpine.sh checkt .mkalpine-done — Paket-
Installation wird uebersprungen wenn schon erledigt, nur ext2 Image wird
neu gebaut. Nach einem Kernel-Fix: ~30s bis zum naechsten Testlauf.

Testsuites im Alpine Image (build/alpine-root/):
- musl libc-test: /opt/libc-test/ (vorgebaut, ~479 Binaries)
- LTP: /opt/ltp/ (vorgebaut, 298 Pflicht-Tests in tools/ltp_required.txt)
- stress-ng: via apk installiert
- Test-Script: tools/boot-test.sh → /opt/boot-test.sh im Image

Separate Build-Pfade (kein Konflikt):
- build/gen/init_bin.h → Kernel init (immer /sbin/init)
- build/test/hw/gen/init_bin.h → ktest (Unit Tests)
- make test-hw aendert nie den Kernel-Init

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
- Jede Aenderung muss automatisch testbar sein
- TEST() fuer Unit-Tests, CRASH_TEST() fuer Adversarial-Tests
- Self-Registering via Linker-Section — kein main.c anfassen
- Kein Test darf haengen: wenn ein Test blockiert, ist das ein Kernel-Bug, kein Test-Bug
- QEMU Serial-Log: /tmp/cosmo-serial.log (User beobachtet das in Echtzeit)

TODO.md:
- Immer aktuell halten. Nach jedem Commit pruefen.
- Erledigte Tasks sofort abhaken oder komprimieren
- Testcount im Header aktualisieren
- Alte erledigte Phasen in Zusammenfassung verschieben

## Ralph-Loop

make alpine-test zeigt den ersten FAIL in /tmp/cosmo-serial.log Analysiere den Fehler, finde den Kernel-Bug, fixe ihn. Committe den Fix. Laufe make alpine-test erneut. Wiederhole bis alle Tests passen oder du nicht weiterkommst. Kein Fragen, kein Warten. Arbeite selbststaendig. test src in build/alpine-root/opt/. CosmoRT ist ein greenfield Projekt mit dem Anspruch mindestens Linux-Niveau und ARINC 653 Partitionierung.
