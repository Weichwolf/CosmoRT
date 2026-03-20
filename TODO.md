# CosmoRT — Audit-Ergebnisse und Anpassungen

Stand: 2026-03-20. Audit gegen CosmoLib/CosmoPX Anforderungen.

Das Gute: Architektur stimmt, Boot korrekt, Syscall-Entry sauber,
Scheduler-Design RT-faehig, ELF-Loader validiert. Fundament ist solide.

Unten die Anpassungen, geordnet nach Abhaengigkeit.
Jeder Block hat ein klares "fertig wenn"-Kriterium.

---

## Phase 1 — Korrektheit (ohne das ist alles andere wertlos)

### 1.1 User-Pointer-Validation

Jeder Syscall der einen User-Pointer dereferenziert ist ein
Kernel-R/W-Exploit. `do_write(buf)`, `do_uname(buf)`,
`do_arch_prctl(ARCH_GET_FS, addr)`, `do_writev(iov)` — alle.

```c
static int user_access_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr; /* overflow */
}
```

Vor jeder User-Pointer-Deref aufrufen. Fehler: `-EFAULT`.
`do_writev` muss sowohl `iov` als auch jedes `iov[i].iov_base` pruefen.

**Fertig wenn:** Kein Syscall dereferenziert unkontrolliert Kernel-Adressen.

### 1.2 FS-Base per Thread (TLS)

Ohne das funktioniert kein Pthread, kein errno, kein Stack Canary.
Zwei Stellen:

1. `thread_t` bekommt `uint64_t fs_base`.
2. `ARCH_SET_FS` schreibt `t->fs_base` UND den MSR.
3. `CLONE_SETTLS` speichert den Wert in `t->fs_base`.
4. Context Switch (`sched_preempt`): aktuellen FS-Base via
   `rdmsr(IA32_FS_BASE)` sichern, neuen laden via `wrmsr`.
5. `proc_enter_ring3` / `thread_run`: FS-Base des Ziel-Threads setzen.

**Fertig wenn:** Zwei Threads mit unterschiedlichem TLS laufen
korrekt nach Preemption. CosmoPX `errno` Test besteht.

### 1.3 getrandom CSPRNG

RDTSC ist kein Zufall. TLS-Session-Keys sind vorhersagbar.

```c
static int do_getrandom(void *buf, size_t len, unsigned int flags) {
    if (!user_access_ok((uint64_t)buf, len)) return -EFAULT;
    for (size_t i = 0; i < len; i += 8) {
        uint64_t r;
        /* RDRAND mit Retry */
        int ok = 0;
        for (int t = 0; t < 10 && !ok; t++)
            asm volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
        if (!ok) return -EIO;
        size_t n = len - i < 8 ? len - i : 8;
        memcpy((uint8_t *)buf + i, &r, n);
    }
    return (int)len;
}
```

CPUID-Check fuer RDRAND (Bit 30 von ECX bei CPUID.01H).
Fallback: RDSEED, oder Fehler. Kein RDTSC-Fallback.

**Fertig wenn:** `getrandom(buf, 32, 0)` liefert kryptographisch
sichere Bytes. CosmoLib TLS-Handshake nutzt echten Zufall.

### 1.4 pages_alloc/pages_free Locking

`pages_alloc` und `pages_free` greifen auf die Bitmap ohne Lock zu.
`page_alloc` hat den Lock, diese beiden nicht.

```c
uint64_t pages_alloc(size_t count) {
    uint64_t flags;
    spin_lock_irq(&page_lock, &flags);
    /* ... bestehende Logik ... */
    spin_unlock_irq(&page_lock, flags);
    return result;
}
```

Analog fuer `pages_free`.

**Fertig wenn:** Concurrent mmap-Stress auf SMP keine doppelten
Pages mehr vergibt.

### 1.5 Futex Blocking

`futex_wait` spinnt 1000 Iterationen und gibt auf. Das ist kein Wait.
Auf Single-Core ist das ein Livelock.

Richtiges Blocking:
1. `futex_wait`: Thread-State auf `THREAD_BLOCKED`, in Futex-Waitqueue
   einhaengen, `thread_return_to_kernel()` → Scheduler waehlt naechsten.
2. `futex_wake`: Threads aus Waitqueue nehmen, State auf `THREAD_READY`,
   in Run-Queue einhaengen.
3. Spurious Wakeup: Aufrufer muss `*uaddr == val` nach Wakeup pruefen
   (macht CosmoPX schon).

**Fertig wenn:** `pthread_mutex_lock` auf kontentierten Mutex blockiert
statt zu spinnen. Single-Core QEMU haengt nicht mehr.

---

## Phase 2 — Fehlende Syscalls (Bootstrap-kritisch)

Reihenfolge nach Abhaengigkeit im Bootstrap:
CosmoPX libc → CosmoLib → fetch → Shell → GCC → Ruby → Homebrew.

### 2.1 Filesystem (VFS)

Ohne open/read/write/close/stat laeuft nichts.

Minimaler VFS:
- `struct file { int type; /* REGULAR, DIR, PIPE, SOCKET */ ... }`
- `struct fd_table { struct file *fds[256]; int count; }`
- Pro Prozess eine `fd_table`.

Phase 1: ramfs (alles im RAM, genuegt fuer Bootstrap-Test).
Phase 2: virtio-blk + simples Dateisystem (ext2-read oder eigenes).

Syscalls:
```
SYS_OPEN     → vfs_open(path, flags, mode) → fd
SYS_CLOSE    → vfs_close(fd)
SYS_READ     → vfs_read(fd, buf, count)
SYS_WRITE    → vfs_write(fd, buf, count)  /* stdout/stderr schon da */
SYS_LSEEK    → vfs_lseek(fd, offset, whence)
SYS_FSTAT    → vfs_fstat(fd, statbuf)
SYS_STAT     → vfs_stat(path, statbuf)
SYS_IOCTL    → vfs_ioctl(fd, cmd, arg)  /* TIOCGWINSZ, FIONREAD */
SYS_FCNTL    → vfs_fcntl(fd, cmd, arg)  /* F_GETFL, F_SETFL */
SYS_DUP2     → fd_table_dup(old, new)
SYS_PIPE     → pipe_create() → [read_fd, write_fd]
SYS_GETCWD   → copy cwd to user buf
SYS_CHDIR    → set cwd
```

**Fertig wenn:** `open("/init", O_RDONLY)` → `read()` → `close()` funktioniert
auf einem ramfs mit eingebetteten Dateien.

### 2.2 fork/exec/waitpid

Ohne das keine Shell, kein `./configure`, kein Build-System.

`fork`:
1. Neuen Prozess allozieren, PID zuweisen.
2. Page Tables kopieren (Copy-on-Write oder erstmal Deep Copy).
3. fd_table duplizieren (alle offenen FDs zeigen auf gleiche file-Structs).
4. Alle Register des Parent in den Child-Kontext kopieren.
5. Child bekommt RAX=0, Parent bekommt RAX=child_pid.

`execve`:
1. ELF laden (existiert schon als `elf_load`).
2. Altes Adressraum-Mapping ersetzen (neue Page Tables).
3. Stack mit argv/envp aufbauen.
4. fd_table beibehalten (ausser FD_CLOEXEC).
5. Einstiegspunkt anspringen.

`waitpid`:
1. Blockieren bis Child exitiert.
2. Exit-Status des Child zurueckgeben.
3. Zombie-Prozess aufraeumen (Pages freigeben — siehe 2.3).

**Fertig wenn:** `fork() + exec("/bin/echo", "hello")` gibt "hello" aus.

### 2.3 Process Cleanup (Exit)

Aktuell leakt alles bei Exit. Kein Page-Free, kein VMA-Free,
kein Thread-Free.

`do_exit(status)`:
1. Alle Threads des Prozesses stoppen.
2. Alle VMAs freigeben → Pages unmap + page_free.
3. Alle Page-Table-Pages freigeben (Walk PML4→PDP→PD→PT).
4. fd_table: alle offenen FDs schliessen.
5. Thread-Structs + Kernel-Stacks freigeben.
6. Prozess-Struct als Zombie markieren (fuer waitpid).
7. Parent benachrichtigen (SIGCHLD oder Waitqueue).

**Fertig wenn:** 1000x `fork+exit` in einer Schleife leakt keinen Speicher.

### 2.4 Signals (minimal)

CosmoPX braucht mindestens:
- `sigaction(SIGSEGV, ...)` — Crash-Handler
- `sigaction(SIGPIPE, SIG_IGN)` — Broken Pipe ignorieren
- `kill(pid, sig)` — Signal senden

Minimale Implementierung:
- Per-Prozess Signal-Handler-Tabelle (64 Eintraege).
- `sigaction`: Handler registrieren.
- Signal-Delivery: Vor Return-to-Userspace pruefen ob Signals pending.
  Wenn ja: User-Stack modifizieren (Signal-Frame pushen), RIP auf Handler.
- `sigreturn`: Signal-Frame vom Stack poppen, Original-Kontext wiederherstellen.

**Fertig wenn:** SIGSEGV-Handler faengt NULL-Deref ab.

### 2.5 Networking (Socket API)

Fuer `fetch https://example.com/` braucht CosmoLib:
```
socket(AF_INET, SOCK_STREAM, 0)
connect(fd, addr, len)
read/write auf Socket-FD
close(fd)
getaddrinfo → ist libc, braucht aber socket()
```

Ansatz: virtio-net Treiber + lwIP oder eigener minimaler TCP/IP Stack.
Alternativ: erstmal nur als Userspace-Treiber via CosmoLib's Netzwerk-Stack.

**Fertig wenn:** `fetch https://example.com/` auf CosmoRT laeuft.

---

## Phase 3 — Robustheit

### 3.1 mmap/munmap Overflow-Check

```c
if (addr + length < addr) return -EINVAL; /* wraparound */
```

In `do_mmap` und `do_munmap`.

### 3.2 proc_create_elf Cleanup bei Failure

Bei jedem Fehlerpfad in `proc_create_elf` muessen bereits allozierte
Pages und Page-Tables freigegeben werden. Goto-Cleanup-Pattern:

```c
pml4 = alloc_page();
if (!pml4) goto fail_slab;
if (elf_load(...) < 0) goto fail_pml4;
if (!thread_alloc(...)) goto fail_elf;
return pid;

fail_elf:   free_address_space(pml4);
fail_pml4:  page_free(pml4);
fail_slab:  slab_free(&proc_slab, p);
            return -ENOMEM;
```

### 3.3 sched_yield implementieren

```c
static long do_sched_yield(void) {
    thread_t *t = current_thread();
    t->state = THREAD_READY;
    sched_enqueue(t);
    thread_return_to_kernel(t); /* longjmp zum Scheduler */
    return 0; /* unreachable */
}
```

### 3.4 TLB Shootdown (SMP)

Bei `munmap`/`mprotect`/Page-Fault-CoW: IPI an alle Cores die den
gleichen Adressraum nutzen (gleiche PML4). Empfaenger-Core fuehrt
`invlpg` aus.

Ohne das ist SMP mit Threads kaputt.

### 3.5 NX-Bit in map_user_page

`map_user_page` muss Protection-Flags respektieren:
- `PROT_EXEC` → kein NX-Bit
- Kein `PROT_EXEC` → NX-Bit setzen
- Kein `PROT_WRITE` → kein PTE_WRITE

### 3.6 Dead Code entfernen

`edf.c` wird nirgends aufgerufen. Entweder einbinden oder entfernen.
`serial_hex64` existiert 3x — einmal in `serial.c` definieren.

---

## Reihenfolge

```
1.1 User-Pointer-Validation     ← Sicherheit, 1 Tag
1.2 FS-Base per Thread           ← Pthreads kaputt ohne, 1 Tag
1.3 getrandom CSPRNG             ← TLS kaputt ohne, halber Tag
1.4 pages_alloc Locking          ← SMP kaputt ohne, halber Tag
1.5 Futex Blocking               ← Pthreads Livelock, 1-2 Tage
2.1 Filesystem (ramfs)           ← Bootstrap-Blocker, 3-5 Tage
2.2 fork/exec/waitpid            ← Shell-Blocker, 3-5 Tage
2.3 Process Cleanup              ← Memory-Leak, 1-2 Tage
2.4 Signals                      ← CosmoPX braucht's, 2-3 Tage
2.5 Networking                   ← fetch-Blocker, 5-10 Tage
3.x Robustheit                   ← Parallel zu Phase 2
```

Phase 1 (Korrektheit) ist Voraussetzung fuer alles andere.
Ein fehlerhafter Scheduler oder kaputtes TLS macht jeden
weiteren Test unzuverlaessig.
