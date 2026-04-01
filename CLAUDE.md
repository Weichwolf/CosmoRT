# CosmoRT

Nativer Realtime-Kernel mit Audio-Fokus. Linux-POSIX-kompatibel, ohne Legacy.

Monolithisch wie Linux, aber ohne 30 Jahre Krusten.
Architektur: Single-Core fuer POSIX-Phase. SMP und RT-Scheduling kommen
zurueck wenn das POSIX-Fundament steht (PREEMPT_RT-Patterns, nicht Microkernel).

Erstes Ziel: 100% Linux/POSIX-Konformitaet (musl, LTP, Alpine).
Einzige Ausnahme: Single-User — kein Multi-User, kein uid/gid-Enforcement.
Danach: RT-Guarantees und Audio-Latenz als Differenzierung.

## Headers

```
include/public/
  cosmort.h   Treiber-API: 5 HW-Primitives, NIC/Block Registration.
  cosmoui.h   CosmoUI-API: Display, Audio, Input, Power (Stubs).

include/kernel/
  core/       sched.h, percpu.h, smp.h, timer.h, timer_wheel.h, irq.h, event_queue.h
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

## Verzeichnisse

```
include/
  public/        cosmort.h (Treiber-API), cosmoui.h (CosmoUI-API)
  kernel/        Kernel-Interna (Subsystem-Spiegel: core/, mm/, proc/, net/, ...)
src/kernel/
  core/          main, irq, sched, timer, timer_wheel, smp, tss, percpu, event_queue
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

## Regeln

Default: Implementiere es wie Linux. Abweichungen NUR wenn durch RT begruendet.

  ┌──────────────────────────────────────────────────────────────────────┬────────────┬──────────────────────────────────────────────────────┐
  │ Regel                                                                │ Linux?     │ CosmoRT-Abweichung                                   │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Stack-Ownership: Ein Thread, ein Kernel-Stack, exklusiv.             │ Wie Linux  │ —                                                    │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ context_switch(prev, next): ein Mechanismus, symmetrisch, atomar.    │ Strenger   │ Linux: asymmetrisch (ret_from_fork). CosmoRT:        │
  │                                                                      │            │ kein Legacy, daher ein symmetrischer Pfad.            │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Atomare Transitions: State-Change und Switch in einer Operation.     │ Wie Linux  │ —                                                    │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Ownership: Ein Owner oder expliziter Refcount. Nie implizit shared.  │ Wie Linux  │ — (Linux: refcount auf file, mm_struct, pages)       │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Ein Pfad pro Konzept: fork/vfork/clone → eine Implementierung.       │ Strenger   │ Linux: 4 Entry-Points → kernel_clone(). CosmoRT:     │
  │                                                                      │            │ kein Legacy, eine Funktion mit Flags.                 │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Bounded Execution: Jeder Kernel-Pfad terminiert in endlicher Zeit.   │ Strenger   │ RT-Anforderung. Linux: unbounded Paths erlaubt.      │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Fail-Stop: Fehler → Panic oder -ERRNO. Nie stille Korruption.        │ Strenger   │ RT: kein Weiterarbeiten mit kaputtem State.           │
  │                                                                      │            │ Linux: WARN_ON + Recovery.                            │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Explizite Dependencies: Jede Abhaengigkeit im Typ/API sichtbar.      │ Strenger   │ Kein Legacy. Linux: initcall-Levels, implizite        │
  │                                                                      │            │ Reihenfolge.                                          │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Interrupt-Transparenz: Jeder Code-Punkt unterbrechbar oder           │ Wie        │ — (konsistent mit PREEMPT_RT)                        │
  │ explizit geschuetzt.                                                 │ PREEMPT_RT │                                                      │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Zero-Copy wo moeglich: Pointer statt Daten bewegen.                  │ Wie Linux  │ — (splice, sendfile, io_uring)                       │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Idempotente Operationen: Syscall-Restart safe, doppeltes Wake No-Op. │ Wie Linux  │ —                                                    │
  └──────────────────────────────────────────────────────────────────────┴────────────┴──────────────────────────────────────────────────────┘

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
