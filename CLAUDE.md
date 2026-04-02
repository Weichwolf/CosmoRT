# CosmoRT

Nativer Realtime-Kernel mit Audio-Fokus. Linux-POSIX-kompatibel, ohne Legacy.

# Kernel Architecture
## POSIX-compatible, single-user, production-grade
### Targets: x86_64 · aarch64 | Userland: Alpine Linux (musl libc)

---

## 0. Grundprinzipien

- Abhängigkeiten nur nach unten. Kein Layer kennt seinen Aufrufer.
- HAL ist die einzige Plattformgrenze. Oberhalb: arch-agnostisch.
- Subsystems kommunizieren über Core-Primitiven, nie lateral.
- POSIX-Compliance bedeutet: fork/exec/wait, signals, file descriptors, mmap, pthreads-ABI.
- Alpine braucht: musl libc, apk (busybox), OpenSSL, OpenSSH, bash, Python — alles läuft im single-user-Kontext über denselben POSIX-Syscall-ABI.

---

## 1. Schichtenmodell

```
┌─────────────────────────────────────────────────────┐
│  Layer 5 │ Userspace                                │
│          │ musl libc · apk · sshd · bash            │
├─────────────────────────────────────────────────────┤
│  Layer 4 │ POSIX Subsystems                         │
│          │ VFS · net (TCP/IP) · IPC · signals       │
├─────────────────────────────────────────────────────┤
│  Layer 3 │ Core Kernel                              │
│          │ scheduler · MM · syscall dispatch        │
├─────────────────────────────────────────────────────┤
│  Layer 2 │ HAL — Hardware Abstraction Layer         │
│          │ irq · timer · cpu · mmu                  │
├─────────────────────────────────────────────────────┤
│  Layer 1 │ Platform / BSP                           │
│          │ boot · SMP init · trampoline             │
├─────────────────────────────────────────────────────┤
│  Layer 0 │ Hardware                                 │
│          │ CPU · MMU · APIC/GIC · timers            │
└─────────────────────────────────────────────────────┘
```

---

## 2. Verzeichnisstruktur

```
kernel/
├── arch/
│   ├── x86_64/
│   │   ├── boot/
│   │   │   ├── entry.S          # long mode entry, initial stack
│   │   │   ├── trampoline.S     # AP real-mode → long-mode (SMP)
│   │   │   └── multiboot.S      # GRUB2 multiboot2 header
│   │   ├── cpu/
│   │   │   ├── gdt.c / gdt.h
│   │   │   ├── idt.c / idt.h
│   │   │   ├── tss.c / tss.h
│   │   │   └── cpuid.c / cpuid.h
│   │   ├── mm/
│   │   │   ├── paging.c / paging.h   # PML4 setup, map/unmap
│   │   │   └── tlb.c / tlb.h         # invlpg, shootdown IPI
│   │   ├── irq/
│   │   │   ├── apic.c / apic.h       # local APIC, IOAPIC
│   │   │   └── pic.c / pic.h         # 8259 fallback
│   │   ├── timer/
│   │   │   ├── hpet.c / hpet.h
│   │   │   └── tsc.c / tsc.h
│   │   ├── smp/
│   │   │   ├── smp.c / smp.h         # BSP→AP bringup
│   │   │   └── ipi.c / ipi.h
│   │   ├── syscall/
│   │   │   ├── entry.S               # SYSCALL/SYSRET path
│   │   │   └── syscall_table.c
│   │   └── context.S                 # save/restore GPRs + SSE
│   │
│   └── aarch64/
│       ├── boot/
│       │   ├── entry.S               # EL2→EL1 drop, stack init
│       │   └── trampoline.S          # secondary CPU entry (PSCI)
│       ├── cpu/
│       │   ├── exceptions.c / exceptions.h  # ESR decode, vector table
│       │   └── sysregs.h                    # SCTLR, TCR, MAIR macros
│       ├── mm/
│       │   ├── paging.c / paging.h   # 4K pages, 3-level PT
│       │   └── tlb.c / tlb.h         # TLBI instructions
│       ├── irq/
│       │   └── gic.c / gic.h         # GICv2/GICv3
│       ├── timer/
│       │   └── arch_timer.c          # Generic Timer (CNTV_CTL)
│       ├── smp/
│       │   └── smp.c                 # PSCI CPU_ON
│       ├── syscall/
│       │   ├── entry.S               # SVC #0 path
│       │   └── syscall_table.c
│       └── context.S                 # save/restore x0-x30 + FPSIMD
│
├── hal/                              # arch-agnostic interfaces
│   ├── hal_irq.h                     # hal_irq_enable/disable/register
│   ├── hal_timer.h                   # hal_timer_set_oneshot/periodic
│   ├── hal_cpu.h                     # hal_cpu_id, hal_cpu_halt, hal_wfi
│   ├── hal_mmu.h                     # hal_mmu_map/unmap/flush
│   └── hal_smp.h                     # hal_smp_boot_ap, hal_ipi_send
│
├── kernel/
│   ├── sched/
│   │   ├── sched.c / sched.h         # pick_next_task, schedule()
│   │   ├── runqueue.c / runqueue.h   # per-CPU runqueue
│   │   ├── policy_rr.c               # round-robin (default)
│   │   └── policy_rt.c               # SCHED_FIFO / SCHED_RR (POSIX)
│   ├── mm/
│   │   ├── pmm.c / pmm.h             # physical page allocator (buddy)
│   │   ├── vmm.c / vmm.h             # virtual address space, mmap
│   │   ├── slab.c / slab.h           # kmalloc / kfree
│   │   ├── vma.c / vma.h             # vm_area_struct, find_vma
│   │   └── fault.c / fault.h         # page fault handler, CoW
│   ├── proc/
│   │   ├── process.c / process.h     # task_struct, alloc/free
│   │   ├── fork.c                    # sys_fork, copy_mm, copy_fd
│   │   ├── exec.c                    # sys_execve, ELF loader
│   │   ├── wait.c                    # sys_wait4, SIGCHLD
│   │   └── signal.c / signal.h       # deliver_signal, sigaction
│   └── syscall/
│       ├── syscall.c                 # syscall_dispatch()
│       └── syscall_nr.h              # NR_read, NR_write, ... (Linux ABI)
│
├── fs/
│   ├── vfs/
│   │   ├── vfs.c / vfs.h             # vfs_open/read/write/close
│   │   ├── dentry.c / dentry.h       # dentry cache
│   │   ├── inode.c / inode.h         # inode ops
│   │   └── mount.c / mount.h         # vfsmount, do_mount
│   ├── ext4/
│   │   ├── ext4.c / ext4.h
│   │   └── ext4_inode.c
│   ├── tmpfs/
│   │   └── tmpfs.c / tmpfs.h
│   ├── procfs/
│   │   └── procfs.c / procfs.h       # /proc/self/maps, /proc/cpuinfo
│   └── devfs/
│       └── devfs.c / devfs.h         # /dev/null, /dev/zero, /dev/tty
│
├── net/
│   ├── socket.c / socket.h           # sock_create, SOCK_STREAM/DGRAM
│   ├── tcp.c / tcp.h                 # TCP state machine
│   ├── udp.c / udp.h
│   ├── ip.c / ip.h                   # IPv4/IPv6 routing
│   ├── netif.c / netif.h             # network interface abstraction
│   └── lo.c                          # loopback
│
├── ipc/
│   ├── futex.c / futex.h             # FUTEX_WAIT/WAKE/REQUEUE (pthreads)
│   ├── pipe.c / pipe.h               # pipe2, splice
│   └── sysv_ipc.c                    # shmget/semget stubs → -ENOSYS
│
├── event/
│   ├── epoll.c / epoll.h             # epoll_create/ctl/wait
│   ├── eventfd.c / eventfd.h         # eventfd2
│   ├── timerfd.c / timerfd.h         # timerfd_create/settime
│   ├── signalfd.c / signalfd.h       # signalfd4
│   └── inotify.c / inotify.h         # inotify_init/add_watch
│
├── vt/
│   ├── vt.c / vt.h                   # virtual terminal, VT100 emulation
│   ├── pty.c / pty.h                 # pseudo-terminal pairs
│   ├── fb.c / fb.h                   # framebuffer console
│   └── input.c / input.h             # keyboard/mouse → VT input queue
│
├── drivers/
│   ├── tty/
│   │   ├── tty.c / tty.h
│   │   └── serial.c                  # UART (x86: 16550, arm: PL011)
│   ├── blk/
│   │   ├── blk.c / blk.h             # block device interface
│   │   ├── virtio_blk.c              # virtio-blk (QEMU/cloud)
│   │   └── nvme.c                    # NVMe (production)
│   ├── net/
│   │   ├── virtio_net.c
│   │   └── e1000.c
│   ├── input/
│   │   └── ps2kbd.c                  # x86 only
│   ├── audio/
│   │   ├── audio.c / audio.h         # audio device interface, ring buffer
│   │   ├── hda.c                     # Intel HD Audio
│   │   └── virtio_snd.c             # virtio-sound (QEMU)
│   └── gpu/
│       ├── gpu.c / gpu.h             # display/render interface
│       └── virtio_gpu.c              # virtio-gpu (QEMU)
│
├── devfs/
│   └── devfs.c / devfs.h             # /dev/null, /dev/zero, /dev/urandom,
│                                      # /dev/fb0, /dev/snd/*, /dev/tty
│
└── include/
    ├── kernel/
    │   ├── types.h                   # u8/u16/u32/u64, pid_t, off_t
    │   ├── errno.h                   # ENOENT, EACCES, ... (Linux values)
    │   ├── list.h                    # intrusive doubly-linked list
    │   ├── spinlock.h                # spinlock_t, spin_lock/unlock
    │   ├── mutex.h                   # mutex_t, sleep-capable
    │   ├── atomic.h                  # atomic_t, atomic_inc/dec/cmpxchg
    │   ├── printk.h                  # printk(KERN_INFO ...)
    │   └── panic.h                   # panic(), BUG_ON(), WARN_ON()
    └── uapi/
        ├── syscall_nr.h              # shared with userspace
        ├── stat.h                    # struct stat (musl-compatible layout)
        ├── mman.h                    # PROT_*, MAP_*, mmap flags
        └── signal.h                  # struct sigaction, SA_*, SIG*
```

---

## 3. Kritische Interfaces

### HAL (hal/*.h)

```c
/* hal/hal_irq.h */
typedef void (*irq_handler_t)(int irq, void *data);
void hal_irq_register(int irq, irq_handler_t fn, void *data);
void hal_irq_enable(int irq);
void hal_irq_disable(int irq);
void hal_irq_mask_all(void);          // used during panic

/* hal/hal_mmu.h */
int  hal_mmu_map(pgdir_t *pgd, vaddr_t va, paddr_t pa, size_t sz, uint32_t flags);
int  hal_mmu_unmap(pgdir_t *pgd, vaddr_t va, size_t sz);
void hal_mmu_flush(vaddr_t va, size_t sz);   // TLB shootdown cross-CPU
pgdir_t *hal_mmu_alloc_pgdir(void);
void     hal_mmu_free_pgdir(pgdir_t *pgd);

/* hal/hal_timer.h */
void hal_timer_set_oneshot(uint64_t ns);     // per-CPU timer
void hal_timer_set_periodic(uint64_t ns);
uint64_t hal_timer_now_ns(void);             // monotonic clock

/* hal/hal_cpu.h */
int  hal_cpu_id(void);
void hal_cpu_halt(void);                     // HLT / WFI
void hal_cpu_full_barrier(void);             // mfence / dsb sy
void hal_cpu_context_switch(struct context *from, struct context *to);
```

### Core: task_struct

```c
/* kernel/proc/process.h */
struct task_struct {
    pid_t           pid;
    pid_t           ppid;
    enum task_state state;          // RUNNING, INTERRUPTIBLE, ZOMBIE, ...
    int             exit_code;

    struct context  *ctx;           // arch-specific saved registers
    struct mm_state *mm;            // address space
    struct fd_table *fds;           // file descriptor table
    struct sig_state *signals;      // pending signals, sigaction table

    struct list_head sched_node;    // runqueue linkage
    uint64_t        runtime_ns;     // for scheduler accounting
    int             sched_policy;   // SCHED_NORMAL / FIFO / RR
    int             priority;

    char            name[32];
};

struct task_struct *task_alloc(const char *name);
void                task_free(struct task_struct *t);
void                task_exit(struct task_struct *t, int code);
```

### Core: mm

```c
/* kernel/mm/vmm.h */
struct mm_state {
    pgdir_t          *pgd;
    struct list_head  vma_list;     // sorted by va
    vaddr_t           brk;
    vaddr_t           stack_top;
    spinlock_t        lock;
};

struct vm_area {
    vaddr_t           start, end;
    uint32_t          prot;         // PROT_READ | PROT_WRITE | PROT_EXEC
    uint32_t          flags;        // MAP_PRIVATE | MAP_SHARED | MAP_ANON
    struct file      *file;         // NULL for anonymous
    off_t             file_offset;
    struct list_head  node;
};

struct mm_state *mm_alloc(void);
struct mm_state *mm_copy(struct mm_state *src);  // for fork, CoW
void             mm_free(struct mm_state *mm);
int              mm_mmap(struct mm_state *mm, vaddr_t hint, size_t sz,
                         uint32_t prot, uint32_t flags,
                         struct file *f, off_t off, vaddr_t *out);
int              mm_munmap(struct mm_state *mm, vaddr_t va, size_t sz);
void             mm_handle_fault(struct mm_state *mm, vaddr_t va,
                                 bool write, bool user);
```

### Core: VFS

```c
/* fs/vfs/vfs.h */
struct file_ops {
    int     (*open)   (struct inode *, struct file *);
    ssize_t (*read)   (struct file *, char *buf, size_t n, off_t *pos);
    ssize_t (*write)  (struct file *, const char *buf, size_t n, off_t *pos);
    int     (*close)  (struct file *);
    int     (*ioctl)  (struct file *, unsigned long cmd, void *arg);
    int     (*mmap)   (struct file *, struct vm_area *);
    off_t   (*lseek)  (struct file *, off_t off, int whence);
    int     (*poll)   (struct file *, struct poll_table *);
};

struct inode_ops {
    struct inode *(*lookup) (struct inode *dir, const char *name);
    int           (*create) (struct inode *dir, const char *name, mode_t);
    int           (*mkdir)  (struct inode *dir, const char *name, mode_t);
    int           (*unlink) (struct inode *dir, const char *name);
    int           (*rename) (struct inode *old_dir, const char *old_name,
                             struct inode *new_dir, const char *new_name);
    int           (*stat)   (struct inode *, struct stat *);
};

int   vfs_open  (const char *path, int flags, mode_t mode, struct file **out);
int   vfs_close (struct file *f);
ssize_t vfs_read  (struct file *f, char *buf, size_t n);
ssize_t vfs_write (struct file *f, const char *buf, size_t n);
int   vfs_mount (const char *dev, const char *path, const char *fstype, unsigned long flags);
```

### Core: Syscall-Dispatch

```c
/* kernel/syscall/syscall.c */
// Wird direkt aus arch/*/syscall/entry.S aufgerufen.
// Auf x86_64: rax=nr, rdi/rsi/rdx/r10/r8/r9 = args.
// Auf aarch64: x8=nr, x0-x5 = args.

long syscall_dispatch(long nr, long a0, long a1, long a2,
                      long a3, long a4, long a5) {
    if (nr >= NR_SYSCALL_MAX) return -ENOSYS;
    return syscall_table[nr](a0, a1, a2, a3, a4, a5);
}

// syscall_nr.h: Linux-kompatible Nummern (musl erwartet sie exakt so)
#define NR_read       0    // x86_64
#define NR_write      1
#define NR_open       2
#define NR_close      3
#define NR_mmap       9
#define NR_munmap     11
#define NR_fork       57
#define NR_execve     59
#define NR_exit       60
#define NR_wait4      61
#define NR_kill       62
#define NR_sigaction  13
#define NR_clone      56
#define NR_futex      202
// aarch64: andere Nummern, gleiche Semantik — eigene syscall_table
```

### Scheduler-Kern

```c
/* kernel/sched/sched.h */
void schedule(void);                         // freiwillig / preemption point
void sched_yield(void);
void sched_enqueue(struct task_struct *t);
void sched_dequeue(struct task_struct *t);
void sched_tick(void);                       // aus timer IRQ, prüft preemption
struct task_struct *sched_current(void);     // current task (per-CPU var)

// preemption guard
void preempt_disable(void);
void preempt_enable(void);                   // ruft schedule() wenn nötig
```

---

## 4. POSIX-Compliance: was wirklich zählt

musl libc braucht folgende Syscall-Gruppen korrekt implementiert:

| Gruppe | Syscalls | Kritisch für Alpine |
|--------|----------|---------------------|
| File I/O | read, write, open, close, lseek, stat, fstat, ioctl | apk, bash, alle tools |
| Process | fork, execve, exit, wait4, getpid, getppid | init, sh |
| Memory | mmap, munmap, mprotect, brk | malloc (musl internal) |
| Signals | sigaction, kill, sigprocmask, sigreturn | error handling |
| Time | clock_gettime, nanosleep | TLS, timeout logic |
| IPC | pipe, socketpair, futex | threads (pthreads via futex!) |
| Network | socket, bind, connect, accept, sendmsg, recvmsg | sshd, apk fetch |
| FS | getcwd, chdir, mkdir, unlink, rename, readdir | apk, busybox |
| TTY | ioctl(TIOCGWINSZ), tcsetattr | interactive shell |

**futex ist nicht optional.** musl implementiert pthreads darüber. Ohne korrekte FUTEX_WAIT / FUTEX_WAKE Semantik hängen alle Threads.

---

## 5. Bootsequenz (beide Architekturen)

### x86_64
```
GRUB2 → multiboot2 header (multiboot.S)
  → entry.S: long mode bereits aktiv (GRUB setzt das)
     - Stack einrichten (BSS-Bereich)
     - Frühe GDT laden (flat 64-bit)
     - kmain() aufrufen
kmain():
  1. pmm_init()          // memory map aus multiboot info
  2. vmm_init()          // kernel address space
  3. hal_irq_init()      // APIC / IDT
  4. hal_timer_init()    // HPET oder TSC-deadline
  5. sched_init()        // idle task anlegen
  6. vfs_init()
  7. smp_init()          // APs über trampoline hochfahren
  8. init_process()      // PID 1: /sbin/init (busybox)
  9. schedule()          // nie zurück
```

### aarch64
```
EFI stub / U-Boot → entry.S:
  - EL2 → EL1 drop (wenn in EL2 gestartet)
  - MMU aus, SCTLR_EL1 initialisieren
  - BSS nullen, Stack setzen
  - kmain()
smp_init(): PSCI CPU_ON statt trampoline unter 1MB
  (PSCI kümmert sich um den Mode-Switch)
```

---

## 6. Was hier bewusst fehlt

| Feature | Grund |
|---------|-------|
| Multi-user / capabilities | Single-user, kein DAC/MAC nötig |
| Kernel modules (.ko) | Komplexität ohne Nutzen im definierten Scope |
| DMA-Remapping (IOMMU) | Relevant ab multi-tenant / virtualisierung |
| Swap | Produktionsreif bedeutet hier: genug RAM oder OOM-kill |
| GPU-Treiber | Kein Display-Subsystem im Scope |
| Audit / seccomp | Optional, nachrüstbar über syscall_dispatch hook |

## Build

WICHTIG: Immer `make` VOR `make test-hw/alpine-test/qemu-alpine` ausfuehren.
Makefile trackt keine Header-Dependencies — nach Struct-Aenderungen sonst stale .o Files.

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
  │ context_switch(prev, next): ein Mechanismus, ein Callsite, atomar.   │ Wie Linux  │ ret_from_fork als Resume-Target fuer neue Threads    │
  │                                                                      │            │ ist inherente fork()-Asymmetrie, kein Legacy.        │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Atomare Transitions: State-Change und Switch in einer Operation.     │ Wie Linux  │ —                                                    │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Ownership: Ein Owner oder expliziter Refcount. Nie implizit shared.  │ Wie Linux  │ — (Linux: refcount auf file, mm_struct, pages)       │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Ein Pfad pro Konzept: fork/vfork/clone → eine Implementierung.       │ Strenger   │ Linux: 4 Entry-Points → kernel_clone(). CosmoRT:     │
  │                                                                      │            │ kein Legacy, eine Funktion mit Flags.                │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Bounded Execution: Core-Pfade bounded, I/O timeout-guarded.          │ Strenger   │ RT-Anforderung. Linux: unbounded Paths erlaubt.      │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Fail-Stop: Fehler → Panic oder -ERRNO. Nie stille Korruption.        │ Strenger   │ RT: kein Weiterarbeiten mit kaputtem State.          │
  │                                                                      │            │ Linux: WARN_ON + Recovery.                           │
  ├──────────────────────────────────────────────────────────────────────┼────────────┼──────────────────────────────────────────────────────┤
  │ Explizite Dependencies: Jede Abhaengigkeit im Typ/API sichtbar.      │ Strenger   │ Kein Legacy. Linux: initcall-Levels, implizite       │
  │                                                                      │            │ Reihenfolge.                                         │
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
