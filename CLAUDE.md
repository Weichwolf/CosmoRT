# CosmoRT

Realtime-Microkernel fuer CosmoOS. POSIX-kompatibel, Single-User.
C11, x86/ARM/RISC-V.

## Stack

```
Apps / Python / git / gcc / Homebrew
  |
libc (dieses Repo: libc/)
  |
Kernel Syscalls (dieses Repo: kernel/)
  |
Hardware  x86/ARM/RISC-V
```

## Ziel

Ein POSIX-kompatibler Realtime-Kernel mit eigener libc.
Alles was auf Linux kompiliert laeuft auch auf CosmoRT.

## libc

Eigene libc im Repo. Kein Fork von musl/glibc — from scratch, minimal,
nur was gebraucht wird. Auf Linux testbar (libc kompiliert als
Shared Library die gegen Linux-Syscalls linkt).

### Prioritaet (nach Abhaengigkeiten)

**Phase 1 — Grundlagen (CosmoLib braucht das):**
- string.h: memcpy, memset, memmove, memcmp, strlen, strcmp, strncmp, strchr, strstr, strncat
- stdlib.h: malloc, free, realloc, calloc, abort, exit, atoi, atol, strtol, strtoul, strtod
- stdint.h, stddef.h, stdbool.h, stdarg.h (freestanding, kein Code)
- stdio.h: snprintf, fprintf, fopen, fclose, fread, fwrite, fseek, ftell, fflush, printf
- math.h: sin, cos, tan, sqrt, pow, fabs, ceil, floor, round, log, exp, fmod
- errno.h: errno (thread-local)
- assert.h: assert

**Phase 2 — Threads & I/O (CosmoLib + Netzwerk braucht das):**
- pthread.h: pthread_create, pthread_join, pthread_detach, mutex, condvar, rwlock
- unistd.h: read, write, close, lseek, pipe, dup2, getcwd, chdir, sleep, usleep
- fcntl.h: open, fcntl
- sys/stat.h: stat, fstat, mkdir
- sys/mman.h: mmap, munmap, mprotect
- sys/socket.h: socket, connect, bind, listen, accept, send, recv, setsockopt
- netdb.h: getaddrinfo, freeaddrinfo
- arpa/inet.h: htons, ntohs, inet_pton, inet_ntop
- poll.h: poll
- time.h: clock_gettime, nanosleep
- signal.h: signal, sigaction (minimal)

**Phase 3 — Ecosystem (Python/git/gcc braucht das):**
- dirent.h: opendir, readdir, closedir
- dlfcn.h: dlopen, dlsym, dlclose
- setjmp.h: setjmp, longjmp
- locale.h: setlocale, localeconv (minimal, C/POSIX locale)
- wchar.h: mbrtowc, wcrtomb (minimal)
- sys/wait.h: waitpid
- spawn.h: posix_spawn (statt fork+exec)
- sys/select.h: select (compat, intern auf poll)
- sys/ioctl.h: ioctl (minimal)
- termios.h: tcgetattr, tcsetattr (Terminal)
- grp.h, pwd.h: Stubs (Single-User, kein root)

### Design

- malloc: Size-Class-Segregated (wie cl_heap). mmap-backed.
- stdio: Buffered I/O mit interner Buffer-Verwaltung. No-Copy wo moeglich.
- pthreads: 1:1 Mapping auf Kernel-Threads. futex-basierte Synchronisation.
- errno: Thread-local via __thread.
- Kein Multi-User: getuid() = 0, getpid() = 1, kein passwd/shadow.

### Testbarkeit

libc kompiliert auf Linux als Shared Library (`libcos.so`).
Tests linken gegen `libcos.so` statt gegen glibc.
LD_PRELOAD fuer Integrationstests mit echten Programmen.

```bash
# Baut libc als Shared Library
make libc

# Tests gegen eigene libc
make test-libc

# Python gegen eigene libc (Integrationstest)
LD_PRELOAD=./libcos.so python3 -c "print('hello')"
```

## Kernel (spaeter)

Der Kernel kommt zuletzt. Erst libc, getestet auf Linux.
Dann Kernel, der die gleichen Syscalls implementiert.

### Kernel-Design (Vorplanung)

- Microkernel: IPC, Scheduling, Memory Management im Kernel.
  Alles andere (Filesystem, Netzwerk, GPU) als Userspace-Services.
- RT-Scheduling: EDF oder Fixed-Priority mit Priority-Inheritance.
- IPC: Synchrone Messages (L4-Stil) + Async Ports.
- Memory: Virtual Memory mit Copy-on-Write fork, Shared Memory, mmap.
- Kein Multi-User. Kein Root. Capabilities statt Permissions.

### Syscall-Interface

POSIX-kompatibel. Linux Syscall-Nummern wo moeglich (einfacheres Testen).

Zusaetzliche CosmoRT-spezifische Syscalls fuer:
- RT-Thread-Prioritaeten
- GPU Command-Buffer Submit
- Audio-Buffer Submit (RT-safe, Zero-Copy)
- Display Present (VSync)

## Specs

- IEEE 1003.1-2024 POSIX.1
- POSIX Threads (IEEE 1003.1c)
- POSIX Realtime Extensions (IEEE 1003.1b)
- POSIX Sockets (Berkeley)
- Single UNIX Specification v4 (Subset)

## Regeln

- `make test` muss immer gruen sein.
- Warnings = Errors.
- C11. Kein C++. Kein Assembly ausser in arch/.
- Jede libc-Funktion hat einen Test.
- POSIX-Conformance-Tests (Open POSIX Test Suite) als Ziel.
