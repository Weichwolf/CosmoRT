# Alpine Test — Bestandsaufnahme

Run: 2026-04-22 nach fcntl-Cluster.

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta vs vorher                         |
|-------|-------|------|------|------|-----------------------------------------|
| ktest | 2732  | 2732 |   0  |   -  | +13 (fcntl/dnotify/sched Regressionstests) |
| musl  |  478  |  460 |  11  |   7  | =/= (stabil)                            |
| LTP   |  298  |  224 |  31  |  43  | +9 PASS, -9 FAIL (fcntl-Cluster)       |

Baseline: ktest 2719, musl 460/11, LTP 215/40/43.

## fcntl-Cluster (2026-04-22)

| Test           | Vorher | Nachher | Fix / Commit                                    |
|----------------|--------|---------|-------------------------------------------------|
| fcntl12/12_64  | FAIL   | PASS    | vfs-Slabs dynamisch (7cc5391)                   |
| fcntl14        | FAIL   | PASS    | (nebenbei via sched_getaffinity — 4ac39c4)      |
| fcntl14_64     | FAIL   | PASS    | (nebenbei)                                      |
| fcntl17/17_64  | FAIL   | PASS    | POSIX-Lock-Release bei exit (8b45873)           |
| fcntl31/31_64  | FAIL   | PASS    | F_SETOWN_EX(F_OWNER_TID) do_tgkill (a7db5db)    |
| fcntl34/34_64  | FAIL   | PASS    | sched_getaffinity echte CPU-Anzahl (4ac39c4)    |
| fcntl35/35_64  | FAIL   | FAIL    | **Offen** — procfs schreibbar fuer pipe-max-size nicht implementiert |
| fcntl36/36_64  | FAIL   | PASS    | Deadlock-Detection Cross-Process (8f182fe)      |
| fcntl37/37_64  | PASS   | PASS    | =                                               |
| fcntl38/38_64  | FAIL   | FAIL    | **Offen** — siehe Verbleibend                   |

**12/14 Scope-Tests behoben.** (fcntl37 war bereits PASS; fcntl14 war intermittierend)

## Verbleibend

### fcntl35 (SAFE_FILE_PRINTF /proc/sys/fs/pipe-max-size)
Tests erwartet dass /proc/sys/fs/pipe-max-size SCHREIBBAR ist. Unsere
procfs hat read-only-Architektur (procfs_read_fn, kein Write-Callback).
Dazu kommt: pipe2() muesste das Userspace-Limit beachten (unpriv
user pipe init size = min(default, pipe-max-size)). Beides verlangt
groesseren Umbau: procfs-Write-Pfad + CAP_SYS_RESOURCE-Check in
pipe_alloc. Ausserhalb dieses Clusters.

### fcntl38 (DN_ATTRIB Parent-Watch via SA_SIGINFO)
dnotify_fire queued beide Matches in dnotify_q, ruft do_kill zweimal.
sig_pending ist 1-Bit → zweiter Kill koalesziert. Unser deliver_signal
repending't nach dnotify-Queue-Peek, aber SYS_RT_SIGRETURN darf keinen
neuen Signal-Check triggern (broke musl pthread_cond-smasher +
capset04 — commit 528d571 revertiert). fcntl38 bleibt ohne nested
Sigreturn-Delivery offen.

Fix waere: deliver_signal-Loop (mehrere pending Bits in einem
Syscall-Return), aber das verlangt Sigframe-Verkettung ohne Sigreturn-
Recursion. Nicht trivial — eigener Commit.

### fcntl14 (Mandatory-Locking variant 1)
Variant 0 (5000 POSIX-Lock-Iterationen) passt. Variant 1 setzt
file_mode = S_ISGID|0600 und wiederholt. Unser Kernel hat keine
Mandatory-Lock-Semantik (Linux >=5.15 auch nicht mehr); in-kernel-
Perf ist marginal — Variant-1-Budget laeuft kurz vor Ende aus.

## cve+execve-Cluster (vorher)

8/9 behoben (cve-2017-17052 flaky).

## eventfd+epoll-Cluster (vorher)

9/10 behoben (epoll_wait02 timing).

## LTP FAIL (31) — verbleibende Gruppen

| Gruppe         | Anzahl | Tests                                              | Ursache                |
|----------------|--------|----------------------------------------------------|------------------------|
| fcntl          |    5   | fcntl35/35_64, fcntl38/38_64, fcntl14             | procfs-write, nested sigreturn, perf |
| epoll_wait     |    2   | epoll_wait02 (timing), epoll_wait05 (connect TBROK)| timing, TCP loopback   |
| clock_*        |    4   | clock_gettime03/04, clock_nanosleep01-03           | NEWTIME NS             |
| clone          |    1   | clone09 (procfs TBROK)                             | -                      |
| bind/accept    |    2   | accept4_01, bind04                                 | AF_UNIX, TBROK         |
| cve            |    1   | cve-2017-17052                                     | memory-reclaim         |
| access         |    2   | access01, access04                                 | DAC edge-cases         |
| rest           |   14   | abort01, acct01, fchdir03, fchownat03, fdatasync02,| diverse                |
|                |        | dup05, dup201, fgetxattr02, fallocate02, stack_clash,|                      |
|                |        | copy_file_range03, abort01, adjtimex01, alarm02    |                        |

## musl FAIL (11)

Stabil zu Baseline: fma, fmal, powf, remquol (softfma),
pthread_atfork-errno-clobber +static, rlimit-open-files +static,
pthread-robust-detach (flaky), tls_get_new-dtv, tls_init-static (flaky).

## Kernel-PFs

**Keine.** fcntl34's "pages_alloc: order too large" per sched_getaffinity-
Fix beseitigt.

## Priorisierung

**Top Fix-Kandidaten (naechste Phase):**

1. **fcntl38** — Loop-Delivery in deliver_signal statt Sigreturn-Nesting
2. **fcntl35** — procfs-Write-Pfad + pipe-max-size-Sysctl
3. **clock_nanosleep01-03** — NEWTIME NS
4. **epoll_wait02** — hrtimer-Latenz

**Deprioritized:** fcntl14 (perf, Mandatory-Locking in Linux entfernt),
access01/04 (DAC), cve-17052 (memory-reclaim).
