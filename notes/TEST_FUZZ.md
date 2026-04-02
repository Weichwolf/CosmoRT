# Syscall-Fuzzer — Implementierungsplan

## Ziel

Der Kernel darf bei keiner Kombination von Syscall-Nummer + Argumenten
crashen, haengen oder Zustand korrumpieren. Jeder Syscall returnt entweder
einen gueltigen Wert oder ein negatives errno. Nach N Runden antwortet
der Kernel weiterhin korrekt.

## Architektur

### Integration

Datei: `test/fuzz/test_syscall_fuzz.c`

Registrierung ueber `CRASH_TEST("fuzz/syscall", test_syscall_fuzz)`.
Laeuft als Teil von `make test-hw` (ktest-Suite).

### PRNG

xorshift64 — deterministisch, kein State ausserhalb einer uint64_t.

```c
static uint64_t rng_state;

static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}
```

Seed: `clock_gettime(CLOCK_MONOTONIC)` Nanosekunden-Anteil.
Seed wird vor dem Fuzz-Lauf auf stdout geschrieben:

```
[fuzz] seed=0x1a2b3c4d5e6f7890
```

Reproduktion: Seed-Override ueber globale Variable oder Compile-Time-Define
`-DFUZZ_SEED=0x...`.

### Isolation

Jede Fuzz-Runde laeuft in einem fork()-Child:

```
for round 0..N:
    pid = fork()
    if child:
        execute fuzz sequence
        exit(0)
    parent:
        wait4(pid, &status, 0, NULL)
        if child crashed/signaled → FAIL, log round + seed
        if child timed out → FAIL (watchdog)
```

Watchdog: Parent setzt nanosleep-basiertes Timeout. Nach 500ms ohne
Child-Exit: `kill(pid, SIGKILL)`, `wait4`, FAIL.

### Konfiguration

| Parameter      | Default | Beschreibung                        |
|----------------|---------|-------------------------------------|
| FUZZ_ROUNDS    | 500     | Runden pro Test-Invocation          |
| FUZZ_CALLS     | 50      | Syscalls pro Runde                  |
| FUZZ_SEED      | 0       | 0 = auto (clock), sonst fix        |
| FUZZ_TIMEOUT_MS| 500     | Watchdog pro Runde in ms            |

## Argument-Generatoren

Wiederverwendbare Generatoren fuer typische Argument-Klassen.

### fd_gen()

Erzeugt File-Deskriptoren aus:

| Wert           | Warum                                      |
|----------------|--------------------------------------------|
| -1             | Kanonisch ungueltig                        |
| -100 (AT_FDCWD)| Spezialbedeutung in *at-Syscalls           |
| 0, 1, 2       | stdin/stdout/stderr — immer offen          |
| 3..10          | Wahrscheinlich offen nach setup            |
| FD_MAX-1 (255)| Grenze                                     |
| FD_MAX (256)   | Eins drueber                               |
| 0x7FFFFFFF     | INT_MAX                                    |
| -2             | Knapp ungueltig                            |
| random 0..1023 | Breite Abdeckung                           |

### ptr_gen()

Erzeugt Pointer aus:

| Wert                      | Klasse              |
|---------------------------|----------------------|
| 0 (NULL)                  | Null-Deref           |
| 0x1                       | Low unmapped         |
| 0xDEAD000000000000        | Non-canonical        |
| 0xFFFF800000000000        | Kernel-Space Anfang  |
| 0xFFFFFFFF80000000        | Kernel .text         |
| 0x7FFFFFFFE000            | Nahe User-Space Ende |
| valid_buf (mmap'd)        | Gueltig              |
| valid_buf + PAGE_SIZE - 1 | Grenzueberlauf       |
| random & 0x7FFFFFFFFFFF   | Zufaellig im Userspace-Range |

Jede Fuzz-Runde allokiert einen 4096-Byte Scratch-Buffer via mmap.
Dieser dient als "gueltiger Pointer" fuer ptr_gen().

### size_gen()

| Wert                | Warum                          |
|---------------------|--------------------------------|
| 0                   | Null-Laenge                    |
| 1                   | Minimal                        |
| 4096 (PAGE_SIZE)    | Seitengrenze                   |
| 4097                | Seitengrenze + 1               |
| 0x7FFFFFFF          | INT_MAX                        |
| 0xFFFFFFFFFFFFFFFF  | SIZE_MAX / -1                  |
| 0x8000000000000000  | MSB gesetzt                    |
| random & 0xFFFFF    | 0..1MB                         |

### flags_gen(valid_mask)

Nimmt die Menge gueltiger Flags und erzeugt:
- 0 (keine Flags)
- Alle gueltigen Flags OR'd
- Einzelne ungueltige Bits (Bits ausserhalb valid_mask)
- random & 0xFFFFFFFF
- -1 (alle Bits)

### path_gen()

| Wert                           | Klasse                |
|--------------------------------|-----------------------|
| NULL                           | Null-Pointer          |
| ""                             | Leerstring            |
| "/"                            | Root                  |
| "/proc/self/status"            | Gueltig               |
| 4095 Bytes 'A'                 | PATH_MAX - 1          |
| 4096 Bytes 'A'                 | PATH_MAX              |
| Kernel-Adresse                 | ptr_gen() Kernel-Werte|
| "/../../../../../../etc/passwd"| Traversal             |
| "a/b/c/d/e/f/g/h/i/j/..."     | Tiefe Verschachtelung |

### sig_gen()

| Wert    | Klasse            |
|---------|-------------------|
| 0       | Ungueltig         |
| 1..31   | Standard-Signale  |
| 32..64  | RT-Signale        |
| -1      | Negativ           |
| 65      | Obergrenze + 1    |
| 999     | Weit draussen     |

### pid_gen()

| Wert          | Klasse                    |
|---------------|---------------------------|
| 0             | Eigene Prozessgruppe      |
| eigene PID    | Selbst                    |
| -1            | Alle Prozesse             |
| -2            | Negative Gruppe           |
| 1             | init (PID 1 = ktest)      |
| PROC_MAX + 1  | Obergrenze                |
| 0x7FFFFFFF    | INT_MAX                   |
| -0x7FFFFFFF   | INT_MIN + 1               |

## Syscall-Klassen

### 1. File-I/O (Prioritaet: HOCH)

**Syscalls:** read(0), write(1), open(2), close(3), lseek(8), openat(257),
readv(19), writev(20), fcntl(72), ioctl(16), dup(32), dup2(33), dup3(292),
getdents64(217), access(21), faccessat(269)

**Argumente:**

| Arg       | Generator  | Spezifisch                              |
|-----------|------------|-----------------------------------------|
| fd        | fd_gen()   | -1, 0, 255, 256, INT_MAX                |
| buf       | ptr_gen()  | NULL, Kernel-Addr, valid                 |
| count     | size_gen() | 0, 1, PAGE_SIZE, SIZE_MAX               |
| offset    | —          | 0, -1, INT64_MAX, INT64_MIN             |
| flags     | flags_gen  | O_RDONLY..O_CLOEXEC, ungueltige Bits     |

**Gefaehrliche Kombinationen:**

- `read(valid_fd, kernel_addr, 4096)` — darf nie in Kernel-Space schreiben
- `write(valid_fd, kernel_addr, 4096)` — darf nie Kernel-Speicher lesen
- `writev(fd, kernel_addr_iovec, 1)` — iovec-Pointer in Kernel-Space
- `read(fd, valid_buf, SIZE_MAX)` — Overflow in size Berechnung
- `lseek(fd, INT64_MIN, SEEK_CUR)` — Unterlauf
- `open(path_4096_bytes, O_RDONLY, 0)` — PATH_MAX Grenze
- `ioctl(fd, 0xFFFFFFFF, kernel_addr)` — beliebiger ioctl-Command
- `fcntl(fd, -1, 0)` — ungueltiger Command
- `dup3(fd, fd, 0)` — same-fd Edge Case
- `getdents64(non_dir_fd, buf, size)` — nicht-Directory FD

**Korrektes Verhalten:** Negativer errno (EBADF, EFAULT, EINVAL, ENAMETOOLONG)
oder gueltiger Return-Wert. Nie Crash, nie Kernel-Memory-Leak.

### 2. Memory (Prioritaet: KRITISCH)

**Syscalls:** mmap(9), munmap(11), mprotect(10), mremap(25), madvise(28),
brk(12), mlock(149), munlock(150), mlockall(151), munlockall(152)

**Argumente:**

| Arg    | Generator   | Spezifisch                                    |
|--------|-------------|-----------------------------------------------|
| addr   | ptr_gen()   | NULL, page-aligned, unaligned, kernel-addr    |
| len    | size_gen()  | 0, 1, PAGE_SIZE, huge, SIZE_MAX               |
| prot   | flags_gen   | Valid: PROT_NONE/READ/WRITE/EXEC, ungueltig: 0xF0 |
| flags  | flags_gen   | MAP_PRIVATE\|MAP_ANONYMOUS, MAP_FIXED auf kernel-addr |
| fd     | fd_gen()    | -1 (anon), gueltig, ungueltig                 |
| offset | —           | 0, unaligned, negative                        |

**Gefaehrliche Kombinationen:**

- `mmap(0, SIZE_MAX, PROT_RW, MAP_PRIV_ANON, -1, 0)` — OOM
- `mmap(kernel_addr, 4096, PROT_RW, MAP_FIXED, -1, 0)` — Kernel-Space ueberschreiben
- `mmap(0, 0, ...)` — Null-Laenge
- `munmap(valid_mapping, SIZE_MAX)` — riesiger Unmap-Range
- `munmap(kernel_addr, 4096)` — Kernel-Mapping loeschen
- `mprotect(kernel_addr, 4096, PROT_RW)` — Kernel-Pages beschreibbar machen
- `mremap(valid, 4096, SIZE_MAX, MREMAP_MAYMOVE)` — unmoeglich grosses Remap
- `mremap(kernel_addr, 4096, 4096, MREMAP_FIXED, user_addr)` — Kernel-Quelle
- `madvise(valid, 4096, 0xFFFF)` — ungueltiger Advice
- `brk(kernel_addr)` — Heap ins Kernel verschieben
- `mmap(0, 4096, PROT_RW, MAP_FIXED, -1, 0)` — Null-Page mappen (gefaehrlich!)

**Korrektes Verhalten:** ENOMEM, EINVAL, EFAULT, oder Erfolg bei gueltigen
Kombinationen. Kernel-Adressen muessen immer abgelehnt werden.

### 3. Process (Prioritaet: HOCH)

**Syscalls:** fork(57), vfork(58), clone(56), clone3(435), execve(59),
wait4(61), exit(60), exit_group(231)

**Argumente:**

| Arg         | Generator  | Spezifisch                              |
|-------------|------------|-----------------------------------------|
| clone_flags | flags_gen  | CLONE_VM, CLONE_THREAD, ungueltige Bits |
| stack       | ptr_gen()  | NULL, kernel-addr, valid                |
| parent_tid  | ptr_gen()  | NULL, kernel-addr                       |
| child_tid   | ptr_gen()  | NULL, kernel-addr                       |
| pid (wait4) | pid_gen()  | -1, 0, INT_MAX, nicht-existente PID     |
| argv (exec) | ptr_gen()  | NULL, kernel-addr, ungueltige Pointer   |

**Gefaehrliche Kombinationen:**

- `clone(CLONE_VM|CLONE_THREAD, kernel_addr, ...)` — Thread mit Kernel-Stack
- `clone(0xFFFFFFFF, valid_stack, ...)` — alle Flags gesetzt
- `clone3(kernel_addr, sizeof(clone_args))` — args in Kernel-Space
- `clone3(valid, SIZE_MAX)` — size Overflow
- `execve(kernel_addr, NULL, NULL)` — Pfad in Kernel-Space
- `execve("/proc/self/exe", [kernel_addr], NULL)` — argv-Entry in Kernel
- `wait4(INT_MAX, kernel_addr, 0, NULL)` — Status in Kernel schreiben
- `wait4(-1, valid, 0xFFFF, NULL)` — ungueltige Flags

**Achtung:** fork() in Fuzz-Runden erzeugt Kinder. Die muessen zuverlaessig
gereapt werden (wait4 nach jedem fork), sonst Zombie-/Proc-Table-Exhaustion.
Max 2-3 Forks pro Runde, immer sofort reapen.

**Korrektes Verhalten:** EINVAL, EFAULT, ENOMEM, ESRCH, ECHILD.

### 4. Signals (Prioritaet: MITTEL)

**Syscalls:** rt_sigaction(13), rt_sigprocmask(14), kill(62), tgkill(234),
sigaltstack(131), rt_sigsuspend(130)

**Argumente:**

| Arg      | Generator  | Spezifisch                            |
|----------|------------|---------------------------------------|
| signum   | sig_gen()  | 0, 9 (SIGKILL), 19 (SIGSTOP), 32, 65 |
| pid      | pid_gen()  | 0, -1, eigene, nicht-existente        |
| act/old  | ptr_gen()  | NULL, kernel-addr                     |
| sigsetsize| —         | 0, 8, 128, SIZE_MAX                   |
| how (mask)| —         | SIG_BLOCK(0), SIG_UNBLOCK(1), SIG_SETMASK(2), 99 |

**Gefaehrliche Kombinationen:**

- `rt_sigaction(SIGKILL, valid_act, NULL, 8)` — SIGKILL ist nicht aenderbar
- `rt_sigaction(0, valid_act, NULL, 8)` — Signal 0 (ungueltig)
- `rt_sigaction(10, kernel_addr, NULL, 8)` — Act in Kernel-Space
- `rt_sigprocmask(99, valid, NULL, 8)` — ungueltiges how
- `rt_sigprocmask(0, NULL, kernel_addr, 8)` — Old-Set in Kernel schreiben
- `kill(0, 0)` — Probe (gueltig, aber Edge Case)
- `kill(-1, SIGKILL)` — Kill an alle Prozesse
- `tgkill(INT_MAX, INT_MAX, 9)` — nicht-existente TIDs
- `sigaltstack(kernel_addr, NULL)` — Stack in Kernel-Space

**Korrektes Verhalten:** EINVAL, EFAULT, ESRCH, EPERM.

### 5. Network (Prioritaet: MITTEL)

**Syscalls:** socket(41), connect(42), bind(49), listen(50), accept(43),
accept4(288), sendto(44), recvfrom(45), sendmsg(46), recvmsg(47),
shutdown(48), setsockopt(54), getsockopt(55), getsockname(51),
getpeername(52), socketpair(53), poll(7)

**Argumente:**

| Arg      | Generator   | Spezifisch                             |
|----------|-------------|----------------------------------------|
| domain   | —           | AF_UNIX(1), AF_INET(2), AF_INET6(10), 0, 999 |
| type     | —           | SOCK_STREAM(1), SOCK_DGRAM(2), 0, 999 |
| protocol | —           | 0, 6 (TCP), 17 (UDP), -1               |
| sockaddr | ptr_gen()   | NULL, kernel-addr, zu kurz, falsche family |
| addrlen  | size_gen()  | 0, 1, sizeof(sockaddr_in), SIZE_MAX    |
| backlog  | —           | 0, 1, -1, INT_MAX                      |
| level    | —           | SOL_SOCKET(1), 0, -1                   |
| optname  | —           | 0, INT_MAX                             |

**Gefaehrliche Kombinationen:**

- `socket(999, 999, 999)` — komplett ungueltige Familie
- `connect(non_socket_fd, valid_addr, len)` — FD ist kein Socket
- `bind(sock_fd, kernel_addr, 16)` — Adresse in Kernel-Space
- `sendto(sock_fd, kernel_addr, 4096, 0, NULL, 0)` — Buffer in Kernel
- `recvfrom(sock_fd, kernel_addr, 4096, 0, NULL, NULL)` — Kernel schreiben
- `sendmsg(sock_fd, kernel_addr_msghdr, 0)` — msghdr in Kernel
- `setsockopt(fd, 1, 0, kernel_addr, 4)` — Optval in Kernel lesen
- `getsockopt(fd, 1, 0, kernel_addr, valid_len)` — Kernel schreiben
- `poll(kernel_addr, 100, 0)` — pollfd-Array in Kernel
- `socketpair(AF_INET, SOCK_STREAM, 0, valid)` — nicht AF_UNIX
- `accept(non_listening_fd, ...)` — FD nicht im Listen-State

**Korrektes Verhalten:** EBADF, ENOTSOCK, EFAULT, EAFNOSUPPORT, EINVAL,
ENOTCONN.

### 6. IPC (Prioritaet: HOCH)

**Syscalls:** pipe(22), pipe2(293), futex(202), eventfd2(290),
epoll_create1(291), epoll_ctl(233), epoll_wait(232), epoll_pwait(281),
timerfd_create(283), timerfd_settime(286), signalfd4(289),
inotify_init1(294), inotify_add_watch(254), inotify_rm_watch(255)

**Argumente:**

| Arg         | Generator  | Spezifisch                         |
|-------------|------------|------------------------------------|
| pipefd      | ptr_gen()  | NULL, kernel-addr                  |
| pipe2_flags | flags_gen  | O_CLOEXEC, O_NONBLOCK, ungueltig   |
| futex_uaddr | ptr_gen()  | NULL, kernel-addr, unaligned       |
| futex_op    | —          | FUTEX_WAIT(0), FUTEX_WAKE(1), 99   |
| futex_val   | —          | 0, 1, INT_MAX, wrong-value         |
| epoll_op    | —          | EPOLL_CTL_ADD(1)..MOD(3), 0, 99    |
| timeout     | —          | NULL, kernel-addr, negative Werte  |

**Gefaehrliche Kombinationen:**

- `pipe(kernel_addr)` — FD-Paar in Kernel schreiben
- `pipe2(kernel_addr, 0)` — dito
- `futex(unaligned_addr, FUTEX_WAIT, 0, NULL, NULL, 0)` — Alignment
- `futex(valid, 99, 0, NULL, NULL, 0)` — ungueltiger op
- `futex(kernel_addr, FUTEX_WAIT, 0, NULL, NULL, 0)` — Kernel-Addr
- `futex(valid, FUTEX_WAIT, wrong_val, timeout=0ms)` — sofort EAGAIN
- `epoll_ctl(epfd, EPOLL_CTL_ADD, epfd, event)` — epoll auf sich selbst
- `epoll_ctl(epfd, EPOLL_CTL_ADD, invalid_fd, event)` — EBADF Target
- `epoll_wait(epfd, kernel_addr, 10, 0)` — Events in Kernel schreiben
- `epoll_wait(epfd, valid, -1, 0)` — negative maxevents
- `epoll_wait(epfd, valid, 0, 0)` — zero maxevents
- `timerfd_settime(tfd, 0, kernel_addr, NULL)` — Spec in Kernel
- `inotify_add_watch(ifd, kernel_addr, 0xFFFFFFFF)` — Pfad + alle Flags
- `eventfd2(0, 0xFFFF)` — ungueltige Flags

**Korrektes Verhalten:** EFAULT, EINVAL, EBADF, ENOENT, EEXIST, EAGAIN.

### 7. FS Metadata (Prioritaet: MITTEL)

**Syscalls:** stat(4), lstat(6), fstat(5), fstatat(262), statfs(137),
fstatfs(138), chmod(90), fchmod(91), fchmodat(268), fchown(93),
link(86), linkat(265), symlink(88), symlinkat(266), readlink(89),
readlinkat(267), unlink(87), unlinkat(263), rename(82), renameat2(316),
mkdir(83), mkdirat(258), rmdir(84), truncate(76), ftruncate(77),
utimensat(280), fallocate(285), mknodat(259), chdir(80), getcwd(79)

**Argumente:**

| Arg      | Generator  | Spezifisch                              |
|----------|------------|-----------------------------------------|
| path     | path_gen() | NULL, leer, kernel-addr, 4096 bytes     |
| statbuf  | ptr_gen()  | NULL, kernel-addr                       |
| dirfd    | fd_gen()   | AT_FDCWD(-100), gueltig, ungueltig      |
| mode     | —          | 0, 0777, 0xFFFFFFFF                     |
| flags    | flags_gen  | AT_SYMLINK_NOFOLLOW, AT_REMOVEDIR, ungueltig |

**Gefaehrliche Kombinationen:**

- `stat(NULL, valid_buf)` — NULL Pfad
- `stat(valid, kernel_addr)` — Buffer in Kernel
- `fstat(INT_MAX, valid_buf)` — riesiger FD
- `fstatat(AT_FDCWD, kernel_addr, valid, 0)` — Pfad in Kernel
- `fstatat(AT_FDCWD, valid, valid, 0xFFFF)` — ungueltige Flags
- `symlink(valid, kernel_addr)` — Ziel in Kernel-Space
- `link(kernel_addr, valid)` — Quelle in Kernel-Space
- `readlink(valid, kernel_addr, 4096)` — Buffer in Kernel
- `readlink(valid, valid, SIZE_MAX)` — Overflow
- `rename(kernel_addr, valid)` — Quelle in Kernel
- `unlinkat(AT_FDCWD, valid, AT_REMOVEDIR|0xFFFF)` — ungueltige Flags
- `truncate(valid, -1)` — negative Laenge
- `ftruncate(fd, INT64_MIN)` — extremer Wert
- `fallocate(fd, 0xFFFF, 0, -1)` — ungueltige Flags, negative Laenge
- `mknodat(AT_FDCWD, valid, 0xFFFF, 0)` — ungueltiger Mode/Typ
- `getcwd(valid, 0)` — Null-Laenge
- `getcwd(valid, 1)` — zu kurz fuer jeden Pfad

**Korrektes Verhalten:** EFAULT, ENOENT, ENOTDIR, ENAMETOOLONG, EINVAL,
EBADF, EEXIST, EPERM.

### 8. Unbekannte Syscalls (Prioritaet: NIEDRIG aber notwendig)

**Syscall-Nummern:**

| Wert                | Beschreibung                    |
|---------------------|---------------------------------|
| -1                  | Negativ                         |
| -0x8000000000000000 | INT64_MIN                       |
| 500                 | Zwischen bekannt und Cosmo-Range|
| 511                 | Knapp unter Cosmo-Bereich       |
| 521..600            | Ueber Cosmo-Bereich             |
| 999                 | Weit draussen                   |
| 0x7FFFFFFFFFFFFFFF  | INT64_MAX                       |

**Argumente:** Alle 6 Argumente zufaellig aus ptr_gen()/size_gen().

**Korrektes Verhalten:** Exakt -ENOSYS (= -38). Kein Crash, kein Hang.

### 9. CosmoRT Hardware Primitives (Prioritaet: HOCH)

**Syscalls:** cosmo_mmio_map(512), cosmo_dma_alloc(513), cosmo_dma_free(514),
cosmo_irq_register(515), cosmo_pci_read(516), cosmo_pci_write(517),
cosmo_fw_load(518), cosmo_nic_attach(519), cosmo_kexec(520)

Normaler Userspace-Prozess (is_driver == false). Alle muessen -EPERM returnen.

**Zusaetzlich testen:** Auch mit gueltigen Argumenten muss -EPERM kommen.
Der Capability-Check muss vor jeder Argument-Validierung stehen.

**Korrektes Verhalten:** -EPERM fuer nicht-Treiber. Kein Crash.

### 10. Stubs und Identity (Prioritaet: NIEDRIG)

**Syscalls:** getuid(102), getgid(104), geteuid(107), getegid(108) → 0.
getpid(39), getppid(110), gettid(186) → positive Werte.
set_robust_list(273) → 0. rseq(334) → -ENOSYS.
capget(125), capset(126) → -EPERM. mount(165), sethostname(170) → 0.

Fuzz: Diese mit beliebigen Argumenten aufrufen. Sie ignorieren Args, duerfen
aber trotzdem nicht crashen wenn unerwartete Werte in den Registern stehen.

## Spezielle Fuzz-Strategien

### S1: Double-Close (Prioritaet: HOCH)

```
fd = open("/proc/self/status", O_RDONLY)
close(fd)
close(fd)   → muss EBADF sein, nicht Use-After-Free
```

Variante: close(fd); dann read(fd, buf, 100) → EBADF.
Variante: close(fd); open(...) bekommt selbe FD-Nummer; close(alte FD) darf
das neue File nicht schliessen.

### S2: Use-After-Unmap (Prioritaet: HOCH)

```
addr = mmap(0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0)
munmap(addr, 4096)
read(fd, addr, 100)  → EFAULT, nicht Crash
write(1, addr, 100)  → EFAULT
mprotect(addr, 4096, PROT_EXEC) → ENOMEM/EINVAL
```

### S3: Concurrent Fuzzing (Prioritaet: MITTEL)

Zwei Threads (clone mit CLONE_VM | CLONE_THREAD) fuzzen gleichzeitig:
- Thread A: oeffnet/schliesst FDs in Schleife
- Thread B: liest/schreibt auf denselben FDs
- Ziel: Race Conditions in fd_table, vfs_file refcount

Implementation: clone() mit shared Memory, geteilter fd_table.
Synchronisation ueber futex oder busy-wait auf shared Variable.

### S4: Signal unter Syscall (Prioritaet: HOCH)

```
rt_sigaction(SIGUSR1, handler, NULL, 8)  // handler = simple return
timer → sendet SIGUSR1 alle 1ms (timerfd + signalfd, oder fork+kill-loop)

Parallel: blockierende Syscalls ausfuehren:
  read(pipe_rd, buf, 1)         → soll EINTR returnen
  epoll_wait(epfd, buf, 10, -1) → soll EINTR returnen
  nanosleep(1sec, remain)       → soll EINTR returnen
  futex(addr, FUTEX_WAIT, ...)  → soll EINTR returnen
  rt_sigsuspend(mask)           → soll EINTR returnen
```

Korrektes Verhalten: EINTR, partieller Return, oder Restart — nie Crash,
nie korrupter Zustand.

### S5: Resource Exhaustion (Prioritaet: HOCH)

**FD-Exhaustion:**
```
for i in 0..FD_MAX+10:
    open("/proc/self/status", O_RDONLY)
→ ab FD_MAX (256): -EMFILE
→ danach: close alle, Kernel hat keine Leaks
```

**Process-Table-Exhaustion:**
```
for i in 0..PROC_MAX+5:
    fork() → child sleeps
→ ab PROC_MAX (16): -EAGAIN
→ danach: kill+wait alle, Tabelle wieder frei
```

**VMA-Exhaustion:** Bereits in test_oom.c, aber Fuzzer soll zusaetzlich
nach OOM weiter Syscalls machen und pruefen dass der Kernel antwortet.

**Pipe-Buffer-Fill:**
```
pipe(fds)
Schreibe in Schleife 4096 Bytes bis write() blockiert oder -EAGAIN (O_NONBLOCK)
→ Kernel darf nicht OOM gehen wegen Pipe-Buffern
```

### S6: Rapid Syscall Cycling (Prioritaet: MITTEL)

1000 Syscalls schnellstmoeglich hintereinander. Mix aus:
- getpid() (0 Args, schnell)
- clock_gettime(CLOCK_MONOTONIC, buf)
- sched_yield()
- write(1, buf, 0)

Prueft: Keine Kernel-Stack-Exhaustion, kein Timer-Drift, keine Deadlocks.

## Runden-Ablauf (Pseudocode)

```
fuzz_round(seed):
    rng_state = seed
    buf = mmap(4096)

    for i in 0..FUZZ_CALLS:
        class = xorshift64() % NUM_CLASSES
        switch class:
            FILE_IO:
                nr = pick_from([0,1,2,3,8,16,19,20,21,32,33,72,217,257,269,292])
                args = generate_args_for_class(nr)
            MEMORY:
                nr = pick_from([9,10,11,12,25,28,149,150,151,152])
                args = ...
            PROCESS:
                nr = pick_from([56,57,59,61])  // kein exit!
                args = ...
            ...
            UNKNOWN:
                nr = random_invalid_nr()
                args = random 6 args

        result = syscall(nr, args...)
        // Invariante: result ist ein long, kein Crash
        // Optional: result validieren (>= -4096 → errno)

    munmap(buf)
    exit(0)
```

## Ergebnis-Validierung

Nach jeder Runde prueft der Parent:

1. **Child lebt:** wait4 returned, kein WIFSIGNALED
2. **Kein Timeout:** Child beendete innerhalb FUZZ_TIMEOUT_MS
3. **Kernel antwortet:** Parent kann nach Runde noch getpid() aufrufen

Nach allen Runden:
4. **Kein Leak:** FD-Count gleich, mmap-Count gleich (via /proc wenn verfuegbar)
5. **Sentinel intakt:** Vorher geschriebener Memory-Wert unveraendert
6. **Timing plausibel:** clock_gettime liefert monoton steigende Werte

## Priorisierung

Reihenfolge nach Wahrscheinlichkeit, Kernel-Bugs zu finden:

1. **Memory-Syscalls** — Pointer-Validierung ist das groesste Risiko
2. **Signal unter Syscall** — Race Conditions im Signal-Delivery-Pfad
3. **File-I/O mit Kernel-Pointern** — copy_from_user/copy_to_user Luecken
4. **IPC (futex, epoll)** — Komplexer State, Race-anfaellig
5. **Double-Close / Use-After-Free** — FD-Lifecycle Bugs
6. **Resource Exhaustion** — Leak-Detection
7. **Process (fork/clone)** — Proc-Table Korruption
8. **Concurrent Fuzzing** — Lock-Contention, Data Races
9. **FS Metadata** — Path-Validierung
10. **Network** — Socket-State-Machine
11. **CosmoRT Primitives** — Capability-Check
12. **Unbekannte Syscalls** — Dispatcher Default-Case
13. **Stubs** — Trivial, aber absichern

## Dateien

| Datei                           | Inhalt                              |
|---------------------------------|-------------------------------------|
| test/fuzz/test_syscall_fuzz.c   | Haupt-Fuzzer, CRASH_TEST            |
| test/fuzz/fuzz_gen.h            | Argument-Generatoren, PRNG          |
| test/fuzz/fuzz_special.h        | S1-S6 Spezialstrategien             |

Alle drei Dateien nutzen nur test/ktest.h, test/syscall.h, test/io.h.
Kein libc, kein Kernel-Internal ausser linux.h (ueber syscall.h).
