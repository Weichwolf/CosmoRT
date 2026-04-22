# Alpine Test — Bestandsaufnahme

Run: 2026-04-21 nach fcntl-Phase 2 (F_SETPIPE_SZ real, SIGIO auf Pipe,
EDEADLK, dnotify mit SA_SIGINFO si_fd, FUTEX_SHARED PA-Key).

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta vs vorher        |
|-------|-------|------|------|------|------------------------|
| ktest | 2694  | 2694 |   0  |   -  | -                      |
| musl  |  478  |  458 |  13  |   7  | -3 (pthread_robust x2 Regression) |
| LTP   |  298  |  198 |  62  |  38  | +11 PASS / -11 FAIL    |

Baseline: ktest 2694, musl 461/10, LTP 187/73/38.

## fcntl-Cluster

Zuwachs: fcntl11/13/15/21/23/27/30/33/39 nun PASS/SKIP.

Verbleibend FAIL (8 Tests + _64 = 16):
- **fcntl12/12_64**: EMFILE-Edge bei F_DUPFD nach rlimit exhaust
- **fcntl14/14_64**: 5000 Random-Iterationen, 10s Timeout zu knapp (perf)
- **fcntl17/17_64**: Deadlock-Detection rc=5 (TBROK — setup-Issue?)
- **fcntl31/31_64**: SIGIO-Delivery via F_SETSIG — Pipe-Owner-Pfad fired,
                      aber Test sigtimedwait sieht nichts (order?)
- **fcntl34/34_64**: Multi-Thread OFD-Lock-Stress, pages_alloc too large
- **fcntl35/35_64**: getpwnam("nobody") Aufruf failt rc=6 (nicht rc=36 SKIP)
- **fcntl36/36_64**: OFD vs POSIX Race (threads)
- **fcntl37/37_64**: F_SETPIPE_SZ spezifischer EBUSY/EPERM-Check
- **fcntl38/38_64**: dnotify DN_ATTRIB — self-watch fires aber Parent nicht?

## Neue Features (diese Phase)

1. **pipe: dynamischer Puffer** (pages_alloc page-multiple) + F_SETPIPE_SZ
   mit EBUSY/EPERM/EINVAL. Default 64KB = Linux PIPE_DEF_BUFFERS.
2. **pipe: SIGIO-Owner** per Ende (reader/writer), F_SETOWN/F_SETSIG/
   O_ASYNC, do_kill-Delivery bei Data-Arrival/Drain.
3. **fcntl F_SETLKW Deadlock-Detection**: Wait-Graph-BFS in flock_waiter_head,
   Depth-Limit 32, OFD-Locks uebergangen (Linux-Semantik).
4. **dnotify**: F_NOTIFY/DN_CREATE/DELETE/RENAME/ATTRIB mit SA_SIGINFO-
   siginfo.si_fd via per-Process-FIFO. VFS-Hooks in mkdir/rmdir/unlink/
   rename/chmod.
5. **FUTEX_SHARED**: Physische Adresse als Key, damit MAP_SHARED|MAP_ANON
   Futexe ueber fork-Grenzen funktionieren (tst_checkpoint).

## Regressionen

- **pthread_robust + pthread_robust-static**: bislang PASS, jetzt FAIL rc=1.
  Wahrscheinlich Interaktion mit FUTEX-Key-Aenderungen fuer FUTEX_PRIVATE vs.
  shared. Tests haben 60s-Internal-Timeout und hitten den; root cause
  noch offen.

## LTP FAIL (62) — verbleibende Gruppen

| Gruppe         | Anzahl | Tests                                           | Ursache                   |
|----------------|--------|-------------------------------------------------|---------------------------|
| fcntl          |   16   | fcntl12/14/17/31/34-38 (+_64)                   | perf, SIGIO-order, TCONF  |
| eventfd/epoll  |    9   | eventfd01-05, epoll_pwait01/04, _wait02/06/07  | EPOLLET/EFD_SEMAPHORE     |
| clock_*        |    4   | clock_gettime03/04, clock_nanosleep01-03        | NEWTIME NS                |
| clone          |    2   | clone09/11 (TBROK procfs/net), clone301         | fixed: 03/05/302          |
| bind/accept    |    6   | accept02/03, accept4_01, bind01-04, connect01   | AF_UNIX, SO_REUSEPORT     |
| chroot/caps/bpf|    7   | chroot01/04, capget01, capset02, bpf_prog02-04  | DAC/capabilities/bpf      |
| cve            |    3   | cve-2016-10044, cve-2017-1705x, cve-2025-38236  | regression tests          |
| access         |    2   | access01, access04                              | DAC edge-cases            |
| rest           |  ~13   | abort01, acct01, adjtimex02, fchdir03, leapsec01, stack_clash, flock03 | diverse |

## musl FAIL (13) — Baseline +3 Regression

Baseline (10): fma, fmal, powf, remquol (softfma), malloc-brk-fail,
pthread_atfork-errno-clobber +static, rlimit-open-files +static,
tls_get_new-dtv.

Neu (3): pthread_robust + pthread_robust-static (Timeout, Regression),
scanf-bytes-consumed (flaky?).

## Kernel-PFs

**Keine.** Nur "pages_alloc: order too large"-Warnung in fcntl34 (threads
mit grossen Stacks + parallel eager-alloc?) und ein SEGFAULT im
bekannten tls_get_new-dtv-Fail.

## Priorisierung (nach fcntl-Phase 2)

**Top Fix-Kandidaten:**

1. **pthread_robust-Regression** (2 tests) — Futex-Key-Mismatch finden
2. **fcntl17 TBROK** (2 tests) — Deadlock-Detection-Setup-Issue
3. **fcntl31 SIGIO** (2 tests) — sigtimedwait sieht pending nicht
4. **eventfd/epoll** (~9 tests) — EPOLLET-Semantik

**Deprioritized:** fcntl14/34/36 (perf), fcntl38 (dnotify si_fd Detail),
chroot/caps/bpf (7 tests), cve/regressions.
