# CosmoRT Design Specification

Referenz-Dokument. Code muss diesem Dokument entsprechen.
Abweichungen sind Bugs.

---

## 1. Hardware-Modell

### 1.1 Core-Topologie

| Core | Rolle | IRQs | Userspace | Locks |
|------|-------|------|-----------|-------|
| 0 | RT-Core | Alle | Nie | Nie shared mit Compute |
| 1..N | Compute | Keine | Immer | Frei |

ARINC 653 Partitionierung:
- RT-Core (Partition A): Bounded WCET, kein Dynamic Alloc, kein Shared Lock
- Compute (Partition B): Best-Effort, COW, Demand-Paging, Slab

Partition-Grenze = SPSC Lock-free Channels. Kein Code-Pfad kreuzt die Grenze.

### 1.2 RT-Core Prioritaeten

```
P0  Audio          <5ms     1 Callback/Tick
P1  Input/HID      <1ms     Drain all
P2  Net-RX         <10ms    max 64 Pakete
P3  Net-TX         <10ms    max 64 Pakete
P4  VSync/DMA      Frame    1 Event
P5  Timer-Wheel    1ms      Alle faelligen
P6  Hash-Engine    Idle     max N Jobs
P7  Page-Age       Idle     max N Pages
P8  KSM Dedup      Idle     max N Pages
```

Handler returniert work_done. Restart von P0 bei work_done > 0. Max 4 Restarts.

### 1.3 RT/Compute Kommunikation

```
rt_channel_t: SPSC Lock-free Ringbuffer
  - Monoton steigende head/tail (uint32_t Wrap-Arithmetik)
  - arch_store_release/arch_load_acquire
  - Power-of-2 Buffer, Modulo via Bitmask
  - 4-Byte Length-Header + Payload pro Message

Richtungen:
  RX:  RT → Compute  (Netzwerk-Daten, Hash-Results)
  TX:  Compute → RT  (Netzwerk-Pakete, Hash-Jobs, Timer-Requests)

IPI: rt_wake(core_id) fuer Cross-Core Wakeup
  - APIC ICR Write, Vector 0xFD, fire-and-forget
  - sched_wake(thread_t *t): atomic BLOCKED→RUNNABLE + IPI
```

### 1.4 Memory Barriers

```
x86 TSO:
  arch_store_release  = Compiler-Barrier (__asm__ volatile("" ::: "memory"))
  arch_load_acquire   = Compiler-Barrier
  arch_wmb/rmb        = Compiler-Barrier
  arch_mb             = mfence

ARM64 (geplant):
  arch_store_release  = stlr
  arch_load_acquire   = ldar
  arch_wmb            = dmb ishst
  arch_rmb            = dmb ishld
  arch_mb             = dmb ish

DMA:
  arch_dma_sync_for_device  x86: No-Op (PCIe cache-koharent)
  arch_dma_sync_for_cpu     x86: No-Op
```

---

## 2. Memory Management

### 2.1 Physische Pages

Buddy-Allocator. Order 0-9 (4KB-2MB).

```
page_alloc(n)     → 2^ceil(log2(n)) Pages, aligned
page_free(ptr, n) → Buddy-Merge
page_incref(phys) → Atomic uint16_t Refcount
page_decref(phys) → Atomic, Free bei 0
```

OOM-Pfad: lazyfree_reclaim(16) → Retry.

### 2.2 Copy-on-Write

PTE_COW = Bit 9 (x86 Software-Bit).

```
fork():
  Fuer jede User-Page:
    Parent PTE: -WRITE +COW
    Child  PTE: -WRITE +COW (kopiert)
    page_incref(phys)
  TLB Flush beider Prozesse

Write-Fault auf COW-Page:
  refcount == 1 → PTE +WRITE -COW (kein Kopieren)
  refcount >  1 → neue Page, memcpy, PTE replace, page_decref(old)
```

### 2.3 MADV_FREE

PTE_LAZYFREE = Bit 10 (x86 Software-Bit).

```
madvise(MADV_FREE):
  Fuer jede Page im Range:
    PTE +LAZYFREE -DIRTY
    Page bleibt gemappt

Erneuter Write:
  CPU setzt DIRTY → Page automatisch gerettet

OOM Reclaim:
  Scan: LAZYFREE + !DIRTY → page_free, PTE=0

Zugriff auf freigegebene Page:
  Demand-Paging → Zero-Page

fork():
  LAZYFREE-Bit gestrippt (COW hat Vorrang)
```

### 2.4 Transparent Huge Pages

PTE_PS = Bit 7 (x86 Hardware). 2MB PMD-Entry, keine Page-Table.

```
mmap(MAP_ANONYMOUS, >= 2MB):
  VMA_HUGEPAGE Flag

Page-Fault an 2MB-Boundary in VMA_HUGEPAGE:
  huge_page_alloc() (Order-9 Buddy)
  Erfolg → PMD-Entry mit PS-Bit
  Fail   → Fallback 4KB

COW-Fault auf Huge Page:
  split_huge_pmd() → 512 × 4KB PTEs
  Dann normaler COW-Fault auf 4KB

munmap/mprotect auf Teil einer Huge Page:
  split_huge_pmd() zuerst, dann Operation auf 4KB
```

### 2.5 KSM (Kernel Same-page Merging)

Laeuft auf RT-Core, niedrigste Prioritaet (P7 Age, P8 Dedup).

```
Interface: MM kennt kein Hashing. Dedup kennt keine PTEs.

MM exportiert:
  mm_dedup_scan_next(phys_out)    Naechste anonyme Cold Page
  mm_dedup_merge(keep, victim)    PTEs umschreiben, page_decref

Dedup exportiert:
  dedup_on_page_free(phys)        Hash invalidieren
  dedup_on_cow_break(old, new)    Hash invalidieren

Page-Age (2-Bit Counter pro Page):
  Scan: Accessed=1 → age=0 (HOT), clear Accessed
  Scan: Accessed=0 → age++ (WARM → COLD)
  Nur PAGE_COLD + !DIRTY werden gehasht

Dedup-Scan:
  scan_next → skip hot → SHA-256 → hash_table_lookup → memcmp → merge
```

---

## 3. Prozess-Modell

### 3.1 Linux ABI

x86_64 ELF. Syscall via `syscall` Instruction.
Register-Konvention: rdi, rsi, rdx, r10, r8, r9 = a1-a6. rax = Syscall-Nr + Return.

Alle SYS_* Nummern exakt Linux x86_64. CosmoRT-eigene ab 0x10000.

### 3.2 FPU/SIMD

FXSAVE/FXRSTOR (512 Bytes). Deckt x87 + MMX + SSE + SSE2 ab.
Kein XSAVE (kein AVX). Ziel: SSE2/NEON (WASM SIMD Limit).

```
Context Switch: fxsave(current) → fxrstor(next)
fork/clone:     fxsave(parent) → memcpy → child
execve:         Reset (MXCSR=0x1F80)
Signal:         fxsave in ucontext, fxrstor bei sigreturn
RT-Core:        Kein Userspace → SSE-Register frei fuer Hash-Engine
```

### 3.3 Permissions

Single-User (UID 0). rwx-Bits gespeichert (chmod), nur +x enforced (exec).
Default: Dirs 0755, Files 0644.

---

## 4. Netzwerk

### 4.1 Architektur

```
src/kernel/net/
  dispatch.c   NIC IRQ → EtherType → Protokoll-Handler
  tcp.c        TCP State-Machine, Per-Socket Ringbuffer
  udp.c        UDP Per-Socket Demux
  arp.c        ARP Cache (Hash 64 Buckets + Pool 128)
  ip.c         IP Header, Checksum, Send
  dns.c        DNS Resolver (Kernel-Boot)
  dhcp.c       DHCP Client (Kernel-Boot)
  socket.c     BSD Socket API
  unix_socket.c  AF_UNIX
```

### 4.2 TCP (RFC 793 + modern)

10 States (RFC 793):
```
CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT1 → FIN_WAIT2 → TIME_WAIT → CLOSED
                     ESTABLISHED → CLOSE_WAIT → LAST_ACK → CLOSED
```

Per-Socket RX-Ringbuffer (64KB, dynamisch alloziert via Buddy).
Hash-Tabelle (256 Buckets, Chaining) fuer tcp_find().

### 4.3 Congestion Control: CUBIC (RFC 8312)

```
W_cubic(t) = C × (t - K)³ + W_max
C = 0.4, Beta = 0.7
K = cbrt(W_max × Beta / C)

Integer-Arithmetik: alle Werte ×1024 fixed-point.
Integer-Kubikwurzel via Newton-Iteration.

Initial Window: 10 × MSS (RFC 6928)
Fast Retransmit: 3 DupACKs → sofort retransmit (RFC 5681 §3.2)
Fast Recovery: cwnd = ssthresh + 3*MSS, inflate bei DupACK
```

### 4.4 SACK (RFC 2018)

SYN: SACK Permitted Option (Kind=4).
ACK: SACK Blocks (Kind=5) bei OOO-Empfang.
Parsing: sack_blocks[4] in net_tcp_t.

### 4.5 Window Scaling (RFC 7323)

SYN: Window Scale Option (Kind=3, Shift=7).
Max Window: 64KB × 2^7 = 8MB.
snd_wnd/rcv_wnd als uint32_t (nicht uint16_t).

### 4.6 Out-of-Order Buffering

4 OOO-Slots pro Connection. ooo_insert bei seq != rcv_nxt.
ooo_drain nach in-order Segment. DupACK bei OOO.

### 4.7 Keepalive

SO_KEEPALIVE via setsockopt. Timer-Wheel auf RT-Core.
Interval: 75s. Max Probes: 9. Probe: seq=snd_nxt-1.

### 4.8 TX-Pfad

```
Compute-Core: send() → tcp_send() → ip_send_raw()
  rt_is_current_rt()? → direkt nic->send()
  sonst              → tx_ring_push(frame)

RT-Core: net_poll() / timer_tick → tx_ring_drain() → nic->send()
```

### 4.9 RX-Pfad

```
RT-Core: NIC IRQ → net_poll() → dispatch
  → tcp_input() → tcp_find() → rxring_push → sched_wake(wait_thread)

Compute-Core: recv() → rxring_pop()
  Leer → sock_block_thread() → schlafen bis IRQ weckt
```

---

## 5. Skalierung

Alle Allokationen O(1). Alle Lookups O(1) amortisiert (Hash) oder O(log n) (AVL).

| Subsystem | Struktur | Lookup | Alloc | Kapazitaet |
|-----------|----------|--------|-------|-----------|
| Socket Pool | Slab + Active-DLL | O(1) | O(1) | 256 |
| TCP Hash | 256 Buckets, Chaining | O(1) avg | O(1) | 256 |
| UDP Hash | 64 Buckets, Chaining + Pool | O(1) avg | O(1) | 128 |
| ARP Cache | 64 Buckets, Chaining + Pool | O(1) avg | O(1) | 128 |
| Timer-Wheel | Free-Stack | — | O(1) | 256 |
| FD-Tabelle | Bitmap + ctzll | — | O(1) POSIX lowest | 1024 |
| VMA | AVL-Tree | O(log n) | O(1) Slab | 8192 |
| PID/TID | Direct-Index Table | O(1) | O(1) | 256/512 |

---

## 6. Crypto

### 6.1 SHA-256 (FIPS 180-4)

Implementierung: src/arch/x86_64/sha256.c (ohne -mno-sse compiliert).
Scalar C. SHA-NI Upgrade geplant (sha256rnds2).

Hash-Engine auf RT-Core (P6). Jobs via SPSC Channel.
Drei Konsumenten: KSM Dedup, CosmoFS Block-Dedup, Cloud-Sync.

---

## 7. Timer

### 7.1 Timer-Wheel

256 Slots, 1ms Granularitaet. Free-Stack fuer O(1) Alloc.
Laeuft auf RT-Core (P5). Compute postet Requests via SPSC Channel.

Actions: TCP Retransmit, TCP Keepalive.

### 7.2 LAPIC Timer

Periodic Mode, Vector 32. Divider 16, Init 10000000.
Feuert auf jedem Core. RT-Core: rt_poll_run(). Compute: sched_preempt().

---

## 8. Treiber-Modell

### 8.1 API (cosmort.h)

5 Primitives + Registration:
```
cosmo_mmio_map, cosmo_dma_alloc, cosmo_irq_register
cosmo_pci_config_read, cosmo_fw_load
nic_driver_t + net_nic_register
blk_driver_t + blk_register
```

Kernel-Treiber: direkter Funktionsaufruf.
Userspace-Treiber: Syscall (0x10000+), HW_CAP_CHECK (is_driver).

### 8.2 Bus-Sortierung

```
src/drivers/
  virtio/    Transport + Geraete (net, blk, gpu, input)
  hyperv/    VMBus + Geraete (netvsc, storvsc, fb, kbd, mouse)
  pci/       Standalone (e1000)
```

### 8.3 Plattform-Support

```
src/arch/x86_64/
  hyperv.c   Hyper-V Detection, MSRs, SynIC, Hypercall-Page
  qemu.c     ACPI Shutdown (PM1a_CNT Ports)
  sha256.c   SHA-256 (ohne -mno-sse)
```

---

## 9. Dateisystem

### 9.1 VFS

Kernel: CosmoFS (Root), ramfs, procfs.
Userspace: FAT32, ext4, NTFS, NFS, SMB (ueber Block-I/O).

### 9.2 CosmoFS v2 (geplant)

Content-Addressed COW. SHA-256 pro 4KB Block.
Keine Journaling (COW = crash-safe). Inline Dedup. Snapshots O(1).
Cloud-Sync: Hash-Diff gegen S3-kompatibles Backend.

---

## 10. Security

### 10.1 Syscall-Haertung

- fault_recover (setjmp/longjmp) fuer alle Kernel-Mode Exceptions
- sigreturn: MXCSR sanitized vor fxrstor
- rt_sigaction: Handler-Adresse validiert (keine Kernel-Adressen)
- mlock/mprotect/madvise: Range-Checks + VMA-Skip (keine O(n) Spin mit IRQs aus)

### 10.2 Test-Strategie

```
make test-hw      Unit-Tests: Happy Path + Edge Cases (912+ Tests)
make test-crash   Adversarial: Kernel darf nie crashen (11+ Tests)
make test-fuzz    Random Syscalls: Survival-Test (1+ Tests)
```

Unit wird bei jedem Feature implementiert.
Crash/Fuzz = Red-Teaming: Agent versucht FAIL zu produzieren.

---

## 11. Boot

UEFI → BOOTX64.EFI → Kernel → VFS → CosmoFS → /etc/cosmo.conf → Init

### 11.1 Shutdown

SYS_REBOOT (169) mit LINUX_REBOOT_MAGIC1/2.
POWER_OFF: arch_shutdown() → ACPI S5 (PM1a_CNT).
RESTART: Triple-Fault → CPU Reset.
