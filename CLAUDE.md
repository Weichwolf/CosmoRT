# CosmoRT

Realtime-Microkernel fuer CosmoOS. POSIX-kompatibel, Single-User.
C11, x86/ARM/RISC-V.

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

## Kernel-Verantwortung

- Scheduling: EDF oder Fixed-Priority mit Priority-Inheritance. RT-faehig.
- Memory: Virtual Memory, Copy-on-Write, Shared Memory, mmap.
- IPC: Synchrone Messages (L4-Stil) + Async Ports.
- Syscalls: POSIX-kompatibel. Linux Syscall-Nummern wo moeglich.

## Userspace-Services (nicht im Kernel)

- Filesystem
- Netzwerk-Stack (TCP/IP)
- GPU-Treiber
- Audio-Treiber
- Input-Treiber

## CosmoRT-spezifische Syscalls

Ueber POSIX hinaus, fuer Multimedia-RT:

- RT-Thread-Prioritaeten (ueber POSIX RT Extensions)
- GPU Command-Buffer Submit (Zero-Copy)
- Audio-Buffer Submit (RT-safe, Zero-Copy)
- Display Present (VSync)
- IPC Fast-Path (Lock-free Message-Passing)

## Single-User

Kein Multi-User. Kein root/sudo. Kein Permission-System.
Capabilities statt File-Permissions.

## Specs

- IEEE 1003.1-2024 POSIX.1 (Syscall-Interface)
- POSIX Realtime Extensions (IEEE 1003.1b)
- POSIX Threads Kernel-Support (1:1 Mapping)

## Regeln

- `make test` muss immer gruen sein.
- Warnings = Errors.
- C11. Assembly nur in arch/ (Boot, Context-Switch, Syscall-Entry).
- Kernel kommt zuletzt. Erst CosmoPX (libc), getestet auf Linux.
