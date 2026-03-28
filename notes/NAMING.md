# CosmoRT Naming & Structure Analysis

## Executive Summary

CosmoRT follows a hybrid naming convention that blends Linux kernel style with custom subsystem-specific prefixing. While the directory structure is well-organized and mirrors Linux conventions, there are inconsistencies in file naming (sys_* prefix inconsistency, vfs_* modularization) and minor reorganization opportunities that would improve navigability without requiring code changes.

### Key Findings
- **Directory structure:** Well-aligned with Linux (kernel/, mm/, fs/, net/, etc. under src/)
- **File naming:** Mixed conventions (some syscalls use `do_` prefix, others use `sys_` prefix)
- **Header locations:** Headers correctly mirrored in include/kernel/
- **Test suite:** Good organization under test/unit/{mm,fs,net,etc.} matching subsystems
- **CosmoRT-specific:** rt.c, rt_poll.c, edf.c (real-time scheduler) appropriately namespaced

---

## Current Structure (Tree View)

```
CosmoRT/
├── src/
│   ├── kernel/              # Core kernel code (mirrors include/kernel/)
│   │   ├── core/            # Scheduler, IRQ, SMP, timers, RT primitives
│   │   ├── mm/              # Memory: paging, slab, page_alloc, vma
│   │   ├── fs/              # Filesystems: VFS, ext2, procfs, bcache
│   │   ├── net/             # Networking: TCP, UDP, ARP, DHCP, DNS
│   │   ├── proc/            # Process management: fork, exec, ELF loading
│   │   ├── sys/             # Syscall layer (biggest subsystem)
│   │   ├── ipc/             # IPC: futex, pipes, sockets
│   │   ├── event/           # Event multiplexing: epoll, eventfd, signalfd
│   │   ├── vt/              # Virtual terminal: PTY, input, framebuffer
│   │   └── hw/              # Hardware: serial, kexec, virtio dispatch
│   ├── drivers/             # Device drivers (userspace)
│   │   ├── virtio/          # Virtio devices: net, blk, gpu, input
│   │   ├── pci/             # PCI drivers: e1000
│   │   └── hyperv/          # Hyper-V devices: VMBus, netvsc, storvsc, keyboard
│   ├── arch/                # Architecture-specific code
│   │   ├── x86_64/          # x86_64: entry.asm, irq, syscall, hyperv, qemu
│   │   ├── aarch64/         # ARM64: entry.S
│   │   └── riscv64/         # RISC-V: entry.S
│   ├── boot/                # UEFI bootloader
│   └── user/                # Userspace startup: init.c, crt0.S
│
├── include/
│   ├── kernel/              # Internal kernel headers (mirrors src/kernel/)
│   │   ├── core/            # Core headers: irq.h, smp.h, timer.h, rt.h
│   │   ├── mm/              # Memory headers: paging.h, slab.h, vma.h
│   │   ├── fs/              # FS headers: vfs.h, ext2.h, procfs.h
│   │   ├── net/             # Net headers: tcp.h, udp.h, socket.h
│   │   ├── proc/            # Process headers: process.h, thread.h, elf.h
│   │   ├── sys/             # Syscall header: syscall.h
│   │   ├── ipc/             # IPC headers: futex.h, ipc.h
│   │   ├── event/           # Event headers: epoll.h, fd.h
│   │   ├── vt/              # VT headers: pty.h, fb.h, input.h, vt.h
│   │   ├── hw/              # Hardware headers: serial.h, kexec.h, hyperv.h
│   │   ├── arch/            # Arch: x86_64.h, arch.h
│   │   └── (top-level)      # Shared: config.h, spinlock.h, ring.h, uaccess.h, memops.h
│   ├── linux/               # Linux ABI headers (libc compatibility)
│   └── public/              # Public driver API: cosmort.h, cosmoui.h
│
└── test/
    ├── unit/                # Unit tests (mirrors subsystems)
    │   ├── fs/              # Filesystem tests (10 files)
    │   ├── hw/              # Hardware tests (4 files)
    │   ├── ipc/             # IPC tests (8 files)
    │   ├── mm/              # Memory tests (8 files)
    │   ├── net/             # Network tests (17 files)
    │   ├── proc/            # Process tests (6 files)
    │   ├── sched/           # Scheduler tests (9 files)
    │   ├── signal/          # Signal tests (7 files)
    │   ├── sys/             # Syscall tests (12 files)
    │   └── perf/            # Performance tests (empty)
    ├── crash/               # Crash/security tests (16 files)
    └── fuzz/                # Fuzzing tests (1 file)
```

---

## Complete File Inventory

### src/kernel/core/ (12 files)
```
core/main.c               # Kernel entry point
core/sched.c              # Per-core priority queue scheduler
core/timer.c              # Clock management
core/timer_wheel.c        # Timer wheel implementation
core/irq.c                # Interrupt request handling
core/smp.c                # Symmetric multiprocessing
core/rt.c                 # RT-Core primitives & channel management
core/rt_poll.c            # RT-Core polling mechanisms
core/edf.c                # Earliest Deadline First scheduler (CosmoRT-specific)
core/percpu.c             # Per-CPU data structures
core/event_queue.c        # Event queue management
core/tss.c                # Task State Segment (x86_64)
```

### src/kernel/mm/ (7 files)
```
mm/paging.c               # Page table setup & management
mm/page_alloc.c           # Buddy allocator
mm/page_cache.c           # Page cache for file I/O
mm/slab.c                 # Slab allocator
mm/vma.c                  # Virtual Memory Area management
mm/memops.c               # Memory operations (copy_from_user, etc.)
mm/random.c               # Random number generation (entropy)
```

### src/kernel/fs/ (7 files + 1 internal header)
```
fs/vfs.c                  # VFS core (inode, dentry, file ops)
fs/vfs_lookup.c           # Path lookup, namecache
fs/vfs_dirops.c           # Directory operations
fs/vfs_rw.c               # Read/write operations
fs/vfs_symlink.c          # Symbolic link handling
fs/vfs_ioctls.c           # ioctl support
fs/bcache.c               # Block cache
fs/ext2.c                 # Ext2 filesystem implementation
fs/procfs.c               # Procfs implementation
fs/vfs_internal.h         # VFS internal definitions (used only in vfs_*.c)
```

### src/kernel/net/ (10 files)
```
net/net.c                 # Network core initialization
net/socket.c              # Socket management (BSD socket API)
net/unix_socket.c         # Unix domain sockets
net/ip.c                  # IP layer
net/tcp.c                 # TCP implementation
net/udp.c                 # UDP implementation
net/arp.c                 # ARP protocol
net/dhcp.c                # DHCP client
net/dns.c                 # DNS client
net/dispatch.c            # Network packet dispatch
net/net_port.c            # Network port management
```

### src/kernel/proc/ (6 files)
```
proc/process.c            # Process creation & management
proc/process_fork.c       # Fork implementation
proc/process_exec.c       # Exec implementation
proc/process_wait.c       # Wait syscalls
proc/process_lazy.c       # Lazy process initialization
proc/elf.c                # ELF loader
```

### src/kernel/sys/ (14 files + 2 internal headers) — THE LARGEST SUBSYSTEM
```
sys/dispatch.c            # Syscall dispatcher (main router)
sys/sys_file.c            # File I/O syscalls (open, read, write, etc.)
sys/sys_fs.c              # Filesystem syscalls (stat, mkdir, unlink, etc.)
sys/sys_mem.c             # Memory syscalls (mmap, brk, mprotect, etc.)
sys/sys_proc.c            # Process syscalls (fork, clone, exec, exit, etc.)
sys/sys_sched.c           # Scheduler syscalls (sched_setaffinity, etc.)
sys/sys_signal.c          # Signal syscalls (rt_sigaction, kill, etc.)
sys/sys_signal_frame.c    # Signal frame delivery
sys/sys_signal_handler.c  # Signal handler management
sys/sys_time.c            # Time syscalls (clock_gettime, nanosleep, etc.)
sys/sys_ipc.c             # IPC syscalls (pipe, futex wrappers)
sys/sys_net.c             # Network syscalls (socket, sendmsg, recvmsg)
sys/sys_event.c           # Event syscalls (epoll, eventfd, timerfd)
sys/sys_id.c              # Identity syscalls (getuid, setuid, etc.)
sys/sys_cosmo.c           # CosmoRT-specific syscalls (DMA, IRQ, etc.)
sys/stubs.c               # Unimplemented syscall stubs
sys/internal.h            # Internal helper definitions & forward declarations
sys/syscall_table.h       # Syscall table (X-macro based)
```

### src/kernel/ipc/ (3 files)
```
ipc/futex.c               # Futex implementation
ipc/ipc.c                 # Generic IPC (pipes, queues)
ipc/net_port.c            # Network port allocation
```

### src/kernel/event/ (5 files)
```
event/epoll.c             # Epoll multiplexing
event/eventfd.c           # Eventfd implementation
event/timerfd.c           # Timerfd implementation
event/signalfd.c          # Signalfd implementation
event/inotify.c           # Inotify implementation
```

### src/kernel/vt/ (4 files)
```
vt/vt.c                   # Virtual terminal core
vt/pty.c                  # Pseudo-terminal implementation
vt/fb.c                   # Framebuffer management
vt/input.c                # Input device handling
```

### src/kernel/hw/ (4 files)
```
hw/serial.c               # Serial port output (kernel logging)
hw/serial_bridge.c        # Serial protocol bridge
hw/kexec.c                # Kexec reboot
hw/cosmort.c              # CosmoRT hardware primitives implementation
```

### src/arch/ (9 files + 3 asm files)
```
arch/x86_64/
  - entry.asm             # Boot entry, interrupt vectors
  - syscall_entry.asm     # Syscall entry point
  - irq_asm.asm           # IRQ assembly stubs
  - context.asm           # Context switching
  - ap_trampoline.asm     # AP startup trampoline
  - kexec_tramp.asm       # Kexec trampoline
  - hyperv.c              # Hyper-V support
  - qemu.c                # QEMU-specific initialization
  - sha256.c              # SHA256 crypto
arch/aarch64/
  - entry.S               # ARM64 entry point
arch/riscv64/
  - entry.S               # RISC-V entry point
```

### src/drivers/ (18 files)
```
drivers/virtio/
  - virtio.c              # Virtio common code
  - virtio.h
  - virtio_blk.c          # Virtio block device
  - virtio_blk.h
  - virtio_net.c          # Virtio network device
  - virtio_net.h
  - virtio_gpu.c          # Virtio GPU
  - virtio_gpu.h
  - virtio_input.c        # Virtio input device
  - virtio_input.h

drivers/pci/
  - e1000.c               # Intel e1000 NIC
  - e1000.h

drivers/hyperv/
  - vmbus.c               # Hyper-V VMBus
  - vmbus.h
  - storvsc.c             # Hyper-V storage
  - storvsc.h
  - netvsc.c              # Hyper-V networking
  - netvsc.h
  - hv_utils.c            # Hyper-V utilities
  - hv_kbd.c              # Hyper-V keyboard
  - hv_mouse.c            # Hyper-V mouse
  - hyperv_fb.c           # Hyper-V framebuffer
```

### src/boot/ (1 file)
```
boot/boot.c               # UEFI bootloader entry
```

### src/user/ (2 files)
```
user/init.c               # Init process
user/crt0.S               # C runtime startup
```

### include/kernel/ (54 headers, mirroring src/kernel structure)
```
include/kernel/
├── core/{edf.h, event_queue.h, irq.h, percpu.h, rt.h, rt_poll.h, smp.h, timer.h, timer_wheel.h}
├── mm/{page_alloc.h, page_cache.h, paging.h, slab.h, vma.h}
├── fs/{bcache.h, ext2.h, procfs.h, vfs.h}
├── net/{arp.h, dhcp.h, dns.h, ip.h, net.h, net_port.h, net_util.h, socket.h, tcp.h, udp.h, unix_socket.h}
├── proc/{elf.h, process.h, proc_internal.h, thread.h}
├── sys/syscall.h
├── ipc/{futex.h, ipc.h}
├── event/{epoll.h, fd.h}
├── vt/{fb.h, input.h, pty.h, vt.h}
├── hw/{hyperv.h, kexec.h, serial.h}
├── arch/{arch.h, x86_64.h}
├── (top-level) {boot_info.h, config.h, memops.h, random.h, ring.h, spinlock.h, uaccess.h, crypto/sha256.h}
```

### include/linux/ (13 headers - Linux ABI compatibility)
```
abi.h, errno.h, fcntl.h, mman.h, epoll.h, prctl.h, sched.h, signal.h, 
socket.h, stat.h, syscall.h, time.h, types.h
```

### include/public/ (2 headers - public driver API)
```
cosmort.h                 # Hardware primitives for drivers
cosmoui.h                 # UI primitives (if applicable)
```

### test/ (89 test files)
```
test/unit/
  fs/      10 files      # VFS, ext2, procfs tests
  hw/      4 files       # PCI, random, dev nodes
  ipc/     8 files       # Futex, pipes, sockets, epoll, etc.
  mm/      8 files       # Paging, slab, CoW, huge pages, mremap
  net/     17 files      # TCP, UDP, ARP, DNS, DHCP, sockets
  proc/    6 files       # Process, procfs, ELF
  sched/   9 files       # Scheduling, TLS, threads, RT channel
  signal/  7 files       # Signals, sigaltstack, job control
  sys/     12 files      # ABI, security, identity, time, prctl
  perf/    (empty)
test/crash/  16 files    # Stack overflow, OOM, resource exhaustion, security
test/fuzz/   1 file      # Syscall fuzzing
```

---

## Comparison with Linux Conventions

| CosmoRT Location | Linux Equivalent | Match? | Notes |
|---|---|---|---|
| src/kernel/ | kernel/ | ✅ YES | Both flat top-level, subdirs match |
| src/kernel/core/ | kernel/ (mixed) | ~PARTIAL | core/ combines core/ + sched/ + timer/ |
| src/kernel/mm/ | mm/ | ✅ YES | Identical: paging, slab, page_alloc, vma |
| src/kernel/fs/ | fs/ | ✅ YES | Both have VFS + filesystems |
| src/kernel/net/ | net/ | ✅ YES | Same structure: tcp, udp, socket, arp |
| src/kernel/proc/ | kernel/ (fork.c, exec.c) | ~PARTIAL | Process logic spread, we group in proc/ |
| src/kernel/sys/ | kernel/sys.c | **DIFFERS** | Linux: single sys.c; CosmoRT: 14 sys_*.c |
| src/kernel/ipc/ | kernel/ipc/ | ✅ YES | Both have futex, pipes |
| src/kernel/event/ | kernel/ + fs/eventpoll/ | ~PARTIAL | Linux spreads epoll/eventfd across dirs |
| src/kernel/vt/ | drivers/tty/ | **DIFFERS** | CosmoRT centralizes PTY/VT in kernel |
| src/kernel/hw/ | drivers/ (early_printk, etc.) | **DIFFERS** | CosmoRT keeps serial + hardware dispatch |
| src/arch/x86_64/ | arch/x86/kernel/ | ✅ YES | Both have entry, syscall, irq asm |
| src/drivers/ | drivers/ (but CosmoRT drivers are userspace) | **DIFFERS** | CosmoRT drivers are userspace apps |
| include/kernel/ | include/linux/ | ~PARTIAL | CosmoRT mirrors kernel/, Linux mirrors arch |
| include/linux/ | include/uapi/linux/ | ✅ YES | Both are ABI headers |
| include/public/ | (N/A) | UNIQUE | CosmoRT-specific public API |

**Key Divergences from Linux:**
1. **sys/ subsystem:** Linux has monolithic kernel/sys.c; CosmoRT splits by category (sys_file.c, sys_mem.c, etc.) — more modular
2. **VT/PTY location:** Linux in drivers/tty/; CosmoRT in kernel/vt/ — makes sense since VT is core
3. **Userspace drivers:** CosmoRT has src/drivers/ (userspace); Linux has drivers/ (kernel modules)
4. **Syscall naming:** CosmoRT uses `sys_*` prefix; Linux uses `do_*` prefix (or no prefix)

---

## Detailed Analysis: src/kernel/sys/ (The Largest Subsystem)

### Current Organization (14 implementation files + 2 headers)

```
sys/
├── dispatch.c              # Main dispatcher (997 lines est.)
├── sys_file.c              # 89 syscalls: open, read, write, lseek, fcntl, etc.
├── sys_fs.c                # FS syscalls: stat, mkdir, unlink, rename, chmod, etc.
├── sys_mem.c               # Memory: brk, mmap, munmap, mprotect, etc.
├── sys_proc.c              # Process: fork, clone, exec, exit, wait, prctl, etc.
├── sys_sched.c             # Scheduler: sched_setaffinity, sched_yield, etc.
├── sys_signal.c            # Signal delivery & handling
├── sys_signal_frame.c      # Signal stack frame setup
├── sys_signal_handler.c    # Signal handler management
├── sys_time.c              # Time: clock_gettime, nanosleep, getitimer, etc.
├── sys_ipc.c               # IPC: pipes, futex (delegated)
├── sys_net.c               # Network syscalls: socket, sendmsg, recvmsg, etc.
├── sys_event.c             # Events: epoll, eventfd, timerfd, signalfd
├── sys_id.c                # Identity: getuid, setuid, getgid, etc.
├── sys_cosmo.c             # CosmoRT-specific: DMA, IRQ, MMIO
├── stubs.c                 # Unimplemented stubs (30+ syscalls)
├── internal.h              # Internal definitions & forward decls (310 lines)
└── syscall_table.h         # X-macro-based syscall routing table
```

### Syscall Naming Convention Analysis

**Current naming:**
- Handlers: `do_read()`, `do_write()`, `do_fork()`, etc. (NO prefix)
- Files: `sys_file.c`, `sys_mem.c`, `sys_proc.c` (PREFIX)
- Dispatcher: routes Linux syscall numbers to `do_*` handlers

**Linux convention:**
- Handlers: `sys_read()`, `sys_write()`, `sys_fork()` (SYS_ prefix)
- Files: typically grouped by function (e.g., fork.c, exec.c, signal.c)
- Dispatcher: kernel/sys.c (monolithic)

**CosmoRT's approach is actually MORE organized** than Linux:
- CosmoRT: sys_file.c groups all file-related syscalls together
- Linux: spreads fs/namei.c, fs/stat.c, fs/open.c across filesystem dirs

### Inconsistency Issue in sys/

**File naming inconsistency:**
- sys_file.c, sys_fs.c, sys_mem.c → consistent `sys_` prefix
- dispatch.c, stubs.c → no `sys_` prefix (why?)
- internal.h → no `sys_` prefix (OK, it's internal)

**Recommendation:** Rename `dispatch.c` → `sys_dispatch.c` for consistency.

---

## Naming Inconsistencies Within CosmoRT

### 1. Syscall Handler Naming (INCONSISTENCY #1)

**Files use `sys_*` prefix:**
```
sys/sys_file.c
sys/sys_mem.c
sys/sys_proc.c
```

**But handlers use `do_*` prefix (following Linux):**
```c
long do_read(int fd, void *buf, size_t count);
long do_write(int fd, const void *buf, size_t count);
long do_fork(void);
```

**Why:** This follows Linux convention where handlers are `do_*` but filenames vary. CosmoRT chose to name files by category, not function. This is NOT an error, just different from Linux's split-by-function naming.

**Resolution:** KEEP AS-IS. The `do_*` naming is standard kernel convention. File categorization is superior.

### 2. Dispatcher File Naming (INCONSISTENCY #2)

**Problem:** `dispatch.c` doesn't follow `sys_dispatch.c` naming.

```
sys/dispatch.c          # INCONSISTENT — should be sys_dispatch.c
sys/sys_file.c          # CONSISTENT
sys/sys_mem.c           # CONSISTENT
```

**Fix:** Rename `dispatch.c` → `sys_dispatch.c` (cosmetic, no code change needed)

### 3. VFS Internal Headers (MINOR ISSUE)

**File:** `fs/vfs_internal.h` (only included by vfs_*.c files)

**Issue:** Violates include hierarchy — internal headers shouldn't use naming like `vfs_internal.h`

**Better names:**
- `vfs_internals.h` (plural, more precise)
- Or move to `include/kernel/fs/` as `vfs_internal.h` for consistency with other internal APIs

**Resolution:** Keep as-is (low priority, internal only)

### 4. Process-Related File Placement (POTENTIAL ISSUE)

**Files:** `proc/process*.c` + `fs/procfs.c`

**Naming mismatch:**
- `process.c` — kernel process management
- `procfs.c` — /proc filesystem

Both are "proc" but serve different purposes. Names are clear enough.

**Resolution:** KEEP AS-IS (names are sufficiently distinct)

### 5. Network Dispatch File (POTENTIAL ISSUE)

**File:** `net/dispatch.c` (packet routing)

**Inconsistency:** Both `sys/dispatch.c` and `net/dispatch.c` use same name

**Actual issue:** None, they're in different dirs. But confusing.

**Potential fix:** Rename `net/dispatch.c` → `net_dispatch.c` or `net_rx.c` for clarity

**Resolution:** Consider for Phase 1, but low priority

### 6. VT Subsystem Naming

All VT files are clear:
```
vt/vt.c                 # Core VT
vt/pty.c                # Pseudo-terminal
vt/fb.c                 # Framebuffer
vt/input.c              # Input
```

**Resolution:** NO ISSUES

### 7. Event Subsystem Naming

All named after their functionality:
```
event/epoll.c
event/eventfd.c
event/signalfd.c
event/timerfd.c
event/inotify.c
```

**Resolution:** NO ISSUES

### 8. Hardware Subsystem Naming

```
hw/serial.c             # Serial output (kernel logging)
hw/serial_bridge.c      # Serial protocol bridge
hw/kexec.c              # Kexec reboot
hw/cosmort.c            # CosmoRT HW primitives
```

**Potential issue:** `cosmort.c` should be `hw_cosmo.c` or stay as-is?

**Resolution:** KEEP — cosmort.c is the implementation of the cosmort.h API, clear naming

---

## Proposed Restructuring

### Phase 1: Renames (No code changes, filesystem only)

**Priority: LOW — Cosmetic improvements**

1. **sys/dispatch.c → sys/sys_dispatch.c**
   - Aligns with sys_file.c, sys_mem.c naming
   - Status: Simple rename, no code changes
   - Benefit: Consistency

2. **net/dispatch.c → net/net_dispatch.c** (OPTIONAL)
   - Avoids naming collision concept with sys/dispatch.c
   - Status: Simple rename, no code changes
   - Benefit: Clarity (but minor, both are in different dirs)

3. **fs/vfs_internal.h → include/kernel/fs/vfs_internal.h** (OPTIONAL)
   - Move internal VFS definitions to header location
   - Status: Requires #include path updates
   - Benefit: Cleaner internal API organization

### Phase 2: Minor Reorganizations (No code changes, better structure)

**Priority: MEDIUM — Improves navigation**

1. **Consolidate core/ related files**
   - Current: core/main.c is kernel entry; scheduler in core/sched.c
   - Potential: No change needed, currently well-organized

2. **Split sys/sys_cosmo.c into subsystem** (OPTIONAL)
   - Current: src/kernel/sys/sys_cosmo.c (CosmoRT-specific syscalls)
   - Potential: New dir src/kernel/cosmo/ with cosmo/ syscalls
   - Reason: CosmoRT's custom rt/dma/irq/pci syscalls are different from POSIX
   - Benefit: Clearer separation of CosmoRT extensions vs. POSIX compliance
   - Status: Would require reorganization but NO code changes

3. **Clarify hw/ subsystem**
   - Current: hw/serial.c (kernel logging), hw/cosmort.c (HW API impl)
   - Potential: Rename hw/cosmort.c → hw/cosmo_hw.c for clarity
   - Status: Simple rename
   - Benefit: Matches naming pattern (cosmo_* = CosmoRT-specific)

### Phase 3: Potential Refactorings (Code changes, higher impact)

**Priority: LOW — Only if major restructuring is desired**

1. **sys_signal.c split**
   - Current: 3 files (sys_signal.c, sys_signal_frame.c, sys_signal_handler.c)
   - Issue: Closely related, could be one file
   - Resolution: Already well-separated for modularity, KEEP AS-IS

2. **Modularize sys_file.c**
   - Current: sys_file.c (very large, 500+ lines est.)
   - Potential: Split into sys_file_io.c + sys_file_mgmt.c
   - Status: Would require code refactoring
   - Benefit: Better separation of concerns
   - Note: Only if file grows much larger

3. **Test directory naming**
   - Current: test/unit/{mm,fs,net,etc.} matches subsystems (GOOD)
   - Potential: Rename test/crash/ → test/security/ or test/stress/
   - Resolution: KEEP — crash tests are intentional crash tests

---

## Naming Conventions to Adopt Globally

### 1. Function Naming (Already followed)
```c
do_syscall_name()           // Syscall handlers
subsys_operation()          // Regular functions (process_fork, page_alloc, etc.)
subsys_internal_helper()    // Internal functions (vfs_lookup, slab_alloc_internal)
```

### 2. File Naming Conventions

**Kernel source files (src/kernel/):**
```
subsys/operation.c          // Normal: fs/vfs.c, net/tcp.c
subsys/subsys_aspect.c      // Multi-file subsystem: vfs_lookup.c, vfs_dirops.c
subsys_operation.c          // Cross-subsystem dispatch: net_dispatch.c
```

**Current deviations to normalize:**
- `src/kernel/sys/dispatch.c` → `src/kernel/sys/sys_dispatch.c` (consistency)
- `src/kernel/net/dispatch.c` → optionally `src/kernel/net/net_dispatch.c` (clarity)

### 3. Header File Naming

**Location:** include/kernel/{subsys}/
```
subsys.h                    // Main subsystem header
subsys_aspect.h             // Aspect-specific header: vfs.h, vfs_lookup.h (if split)
subsys_internal.h           // Internal definitions (if present in include/)
```

**Public headers:** include/public/
```
cosmort.h                   // Hardware primitives API
cosmoui.h                   // UI API
linux/                      // Linux ABI compatibility
```

### 4. Test File Naming

**Current (already excellent):**
```
test/unit/{subsys}/test_feature.c    // Functional tests
test/crash/test_*.c                  // Crash/security tests
test/fuzz/test_*.c                   // Fuzzing
```

**Recommendation:** KEEP AS-IS, very clear

---

## What to Keep (CosmoRT-Specific Identity)

### 1. Real-Time Scheduling
```
core/edf.c                  # Earliest Deadline First (unique to CosmoRT)
core/rt.c                   # RT-Core primitives (unique)
core/rt_poll.c              # RT polling (unique)
```
Keep naming as-is. These are CosmoRT innovations.

### 2. CosmoRT-Specific Syscalls
```
sys/sys_cosmo.c             # DMA, IRQ, MMIO, PCI syscalls
include/public/cosmort.h    # Hardware primitives API
```
These should remain clearly labeled as CosmoRT extensions.

### 3. Single-User Semantics
```
sys/sys_id.c                # Single-user system (no real uid/gid)
```
Current naming is fine; semantics are in implementation.

### 4. Virtual Terminal in Kernel
```
kernel/vt/                  # PTY, framebuffer, input (in kernel)
```
Unlike Linux where PTY is in drivers/tty/, CosmoRT puts VT core in kernel. Keep location and naming as-is.

---

## Summary of Recommended Changes

### Must-Do (Phase 1 - Consistency):
1. ✅ Rename: `src/kernel/sys/dispatch.c` → `sys/sys_dispatch.c`
   - Reason: Matches sys_file.c, sys_mem.c naming
   - Effort: File rename only
   - Impact: Consistency

### Should-Do (Phase 2 - Clarity):
1. 🟡 Rename: `src/kernel/net/dispatch.c` → `net/net_dispatch.c` (OPTIONAL)
   - Reason: Avoids naming confusion with sys/dispatch.c
   - Effort: File rename only
   - Impact: Clarity (low priority)

### Nice-To-Do (Phase 3 - Organization):
1. 🔵 Rename: `src/kernel/hw/cosmort.c` → `hw/cosmo_hw.c` (OPTIONAL)
   - Reason: Matches cosmo_* naming pattern
   - Effort: File rename + #include updates
   - Impact: Consistency with cosmo_* naming for CosmoRT-specific code

2. 🔵 Consider: New `src/kernel/cosmo/` directory for CosmoRT-specific syscalls
   - Reason: Separates POSIX compliance from CosmoRT extensions
   - Effort: Directory creation + file moves + include updates
   - Impact: Clearer architecture
   - Status: Only if future growth warrants it

### Keep As-Is:
- ✅ Header structure (include/kernel/ mirrors src/kernel/)
- ✅ Test organization (matches subsystems)
- ✅ File categorization in sys/ (superior to Linux's spread-out approach)
- ✅ VT in kernel/ (not drivers/, appropriate for CosmoRT)
- ✅ CosmoRT-specific subsystems (rt.c, edf.c, cosmort.c)
- ✅ Driver location (src/drivers/ is userspace-only, separate from kernel)

---

## Comparison Table: CosmoRT vs. Linux Naming by Subsystem

| Subsystem | CosmoRT File(s) | Linux Equivalent | Assessment |
|---|---|---|---|
| **Scheduler** | core/sched.c | kernel/sched/core.c | ✅ Similar |
| **Timers** | core/timer.c, core/timer_wheel.c | kernel/time/timer.c | ✅ Similar |
| **IRQ** | core/irq.c | kernel/irq/irqdesc.c | ✅ Similar |
| **Memory allocation** | mm/page_alloc.c, mm/slab.c | mm/page_alloc.c, mm/slub.c | ✅ Identical |
| **Page tables** | mm/paging.c | mm/pgtable.c (arch-specific) | ✅ Similar |
| **VFS core** | fs/vfs.c | fs/namei.c, fs/dcache.c | ✅ Similar |
| **Ext2 FS** | fs/ext2.c | fs/ext2/inode.c | ✅ Similar |
| **Syscalls (file)** | sys/sys_file.c | fs/open.c, fs/read_write.c | 🟡 CosmoRT groups better |
| **Syscalls (memory)** | sys/sys_mem.c | mm/mmap.c, mm/mremap.c | 🟡 CosmoRT groups better |
| **Syscalls (dispatch)** | sys/sys_dispatch.c | kernel/sys.c | 🟡 CosmoRT modularized |
| **Network (core)** | net/net.c, net/socket.c | net/core/sock.c | ✅ Similar |
| **TCP** | net/tcp.c | net/ipv4/tcp.c | ✅ Similar |
| **UDP** | net/udp.c | net/ipv4/udp.c | ✅ Similar |
| **Process mgmt** | proc/{process,fork,exec}.c | kernel/{fork,exec}.c | ✅ Similar |
| **ELF loading** | proc/elf.c | fs/binfmt_elf.c | ✅ Similar |
| **Signals** | sys/sys_signal*.c | kernel/signal.c | ✅ Similar |
| **IPC (futex)** | ipc/futex.c | kernel/futex/core.c | ✅ Similar |
| **Event (epoll)** | event/epoll.c | fs/eventpoll.c | ✅ Similar |
| **PTY** | vt/pty.c | drivers/tty/pty.c | 🔴 Different location (kernel vs drivers) |
| **Framebuffer** | vt/fb.c | drivers/video/fbdev/ | 🔴 Different location |
| **Serial** | hw/serial.c | drivers/tty/serial/ | 🔴 Different location |

### Why CosmoRT's Differences Make Sense:
1. **PTY/VT in kernel vs. drivers/tty:** CosmoRT treats VT as core (needed early); Linux modules it
2. **Modularized sys/:** CosmoRT's sys_file.c, sys_mem.c approach is MORE readable than Linux's scattered *syscalls.c files
3. **Process files grouped:** proc/ directory groups all process-related code; Linux scatters fork.c, exec.c, etc. in kernel/

---

## Recommended File Tree After Phase 1

```
src/kernel/
├── core/
│   ├── main.c
│   ├── sched.c
│   ├── timer.c
│   ├── timer_wheel.c
│   ├── irq.c
│   ├── smp.c
│   ├── rt.c
│   ├── rt_poll.c
│   ├── edf.c
│   ├── percpu.c
│   ├── event_queue.c
│   └── tss.c
├── mm/
│   ├── paging.c
│   ├── page_alloc.c
│   ├── page_cache.c
│   ├── slab.c
│   ├── vma.c
│   ├── memops.c
│   └── random.c
├── fs/
│   ├── vfs.c
│   ├── vfs_lookup.c
│   ├── vfs_dirops.c
│   ├── vfs_rw.c
│   ├── vfs_symlink.c
│   ├── vfs_ioctls.c
│   ├── vfs_internal.h
│   ├── bcache.c
│   ├── ext2.c
│   └── procfs.c
├── net/
│   ├── net.c
│   ├── socket.c
│   ├── unix_socket.c
│   ├── ip.c
│   ├── tcp.c
│   ├── udp.c
│   ├── arp.c
│   ├── dhcp.c
│   ├── dns.c
│   ├── net_dispatch.c        # RENAMED from dispatch.c (optional)
│   └── net_port.c
├── proc/
│   ├── process.c
│   ├── process_fork.c
│   ├── process_exec.c
│   ├── process_wait.c
│   ├── process_lazy.c
│   └── elf.c
├── sys/
│   ├── sys_dispatch.c        # RENAMED from dispatch.c (MUST)
│   ├── sys_file.c
│   ├── sys_fs.c
│   ├── sys_mem.c
│   ├── sys_proc.c
│   ├── sys_sched.c
│   ├── sys_signal.c
│   ├── sys_signal_frame.c
│   ├── sys_signal_handler.c
│   ├── sys_time.c
│   ├── sys_ipc.c
│   ├── sys_net.c
│   ├── sys_event.c
│   ├── sys_id.c
│   ├── sys_cosmo.c
│   ├── stubs.c
│   ├── internal.h
│   └── syscall_table.h
├── ipc/
│   ├── futex.c
│   ├── ipc.c
│   └── net_port.c            # or move to net/?
├── event/
│   ├── epoll.c
│   ├── eventfd.c
│   ├── timerfd.c
│   ├── signalfd.c
│   └── inotify.c
├── vt/
│   ├── vt.c
│   ├── pty.c
│   ├── fb.c
│   └── input.c
└── hw/
    ├── serial.c
    ├── serial_bridge.c
    ├── kexec.c
    └── cosmort.c             # or rename to cosmo_hw.c?
```

---

## Implementation Plan

### If implementing Phase 1 (Must-Do):
```bash
# Rename for consistency
git mv src/kernel/sys/dispatch.c src/kernel/sys/sys_dispatch.c

# Update Makefile includes (if separate)
sed -i 's/dispatch\.o/sys_dispatch.o/g' Makefile
```

### If implementing Phase 2 (Optional):
```bash
# Rename for clarity (optional)
git mv src/kernel/net/dispatch.c src/kernel/net/net_dispatch.c

# Rename for consistency
git mv src/kernel/hw/cosmort.c src/kernel/hw/cosmo_hw.c

# Update Makefile
sed -i 's/dispatch\.o/net_dispatch.o/g' Makefile
sed -i 's/cosmort\.o/cosmo_hw.o/g' Makefile
```

### If implementing Phase 3 (Future):
```bash
# Create new directory for CosmoRT-specific subsystem
mkdir -p src/kernel/cosmo
git mv src/kernel/sys/sys_cosmo.c src/kernel/cosmo/cosmo.c

# Update includes
# (Many files would need to update: #include "sys/sys_cosmo.c" → #include "cosmo/cosmo.h")
```

---

## Conclusion

CosmoRT's naming and structure are **already well-aligned with Linux conventions** and are in many cases **superior** (modularized sys/ syscalls, grouped process files, VT in kernel where it makes sense for a microkernel).

### Recommended Actions:
1. **Phase 1 (MUST):** Rename `sys/dispatch.c` → `sys/sys_dispatch.c` for consistency
2. **Phase 2 (OPTIONAL):** Rename `net/dispatch.c` → `net/net_dispatch.c` for clarity
3. **Phase 3 (FUTURE):** Consider separating CosmoRT-specific code into `src/kernel/cosmo/`

These changes are cosmetic and improve clarity without requiring major code refactoring. CosmoRT's architecture is fundamentally sound and maintainable.
