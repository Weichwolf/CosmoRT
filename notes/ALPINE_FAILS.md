# Alpine Test — Bestandsaufnahme

Run: 2026-04-22 nach eventfd+epoll-Cluster.

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta vs vorher                         |
|-------|-------|------|------|------|-----------------------------------------|
| ktest | 2719  | 2719 |   0  |   -  | +6 (epoll/eventfd-Regressionstests)     |
| musl  |  478  |  460 |  11  |   7  | =/= (stabil)                            |
| LTP   |  298  |  215 |  40  |  43  | +8 PASS, -8 FAIL (eventfd+epoll+clock)  |

Baseline: ktest 2713, musl 460/11, LTP 207/48/43.

## eventfd+epoll-Cluster (Behoben 2026-04-22)

| Test                | Vorher | Nachher | Fix                                       |
|---------------------|--------|---------|-------------------------------------------|
| eventfd01           | FAIL   | PASS    | (probe-remove, siehe unten)               |
| eventfd02           | FAIL   | PASS    | (probe-remove)                            |
| eventfd03           | FAIL   | PASS    | pselect6 Kernel-seitiges pfd-Array        |
| eventfd04           | FAIL   | PASS    | EPOLLOUT nicht ready bei counter==MAX-1   |
| eventfd05           | FAIL   | PASS    | (probe-remove)                            |
| epoll_pwait01       | FAIL   | PASS    | epoll_pwait2 sigmask-Handling             |
| epoll_pwait04       | FAIL   | PASS    | epoll_pwait2 EFAULT auf bad sigmask       |
| epoll_wait06        | FAIL   | PASS    | probe-remove (EPOLLET events=0-Bug)       |
| epoll_wait07        | FAIL   | PASS    | probe-remove (EPOLLONESHOT events=0-Bug)  |
| epoll_wait02        | FAIL   | FAIL    | **Offen** — 500 iter*1ms TBROK (timing)   |

**9/10 Scope-Tests behoben.** epoll_wait02 bleibt offen: tst_timer_test
killt Test nach 39s statt erwarteten ~500ms; Root Cause vermutlich in
hrtimer-Programmierung oder Wake-Latenz (nicht im epoll-Core).

### Root Causes

1. **Probe-Write in do_epoll_wait**: `copy_to_user(events, 0x00, 1)` am
   Funktionsanfang setzte byte 0 des ersten Events auf 0. Bei LTP-Makros
   die TST_EXP_EQ_LI(epoll_wait(...), X) nutzen (doppelte Evaluation!),
   ueberschrieb der zweite Call den ersten Event — leerer Scan schreibt
   nichts, nur der Probe-Zero blieb stehen. User sah events=0 obwohl
   erster Call korrekt events=EPOLLIN/EPOLLOUT lieferte.

2. **pselect6 → do_poll**: do_pselect6 baute pfd[] auf Kernel-Stack
   und rief do_poll(pfd, ...). do_poll macht aber copy_from_user auf
   fds_ptr → -EFAULT fuer Kernel-Adresse. Folge: `select()` in
   eventfd03/04 und vielen anderen Tests lieferte EFAULT statt zu
   funktionieren.

3. **eventfd fd_poll_readiness**: EPOLLOUT wurde immer zurueckgegeben,
   auch wenn counter == MAX-1 und weitere Writes EAGAIN liefern
   wuerden. Linux fs/eventfd.c prueft `counter < EVENTFD_ULLONG_MAX-1`.

4. **epoll_pwait2 war Stub**: do_epoll_pwait2 ignorierte sigmask komplett
   und delegierte direkt an do_epoll_wait → sigmask-Blocking und
   EFAULT-Semantik fehlten, beide epoll_pwait-Tests (variant 1 =
   epoll_pwait2) TFAIL.

5. **clock_gettime 1ms-Granularitaet**: timer_ms() liefert nur
   Millisekunden → CLOCK_MONOTONIC-Aufloesung 1e6 ns. LTP
   tst_timer_test rechnet Thresholds gegen clock_getres; bei 1 ms
   waren viele Timing-Tests flaky oder TFAIL. Jetzt TSC-ns direkt,
   Resolution 1 ns. **Nebenprofit**: leapsec01 PASS.

## cve+execve-Cluster (Behoben vorher)

8/9 behoben (cve-2017-17052 flaky wegen Memory-Stress).

## fcntl-Cluster

Verbleibend FAIL (16): fcntl12/14/17/31/34-38 (+_64). Unveraendert zu
letztem Run — nicht im Scope dieser Phase.

## LTP FAIL (40) — verbleibende Gruppen

| Gruppe         | Anzahl | Tests                                              | Ursache                |
|----------------|--------|----------------------------------------------------|------------------------|
| fcntl          |   16   | fcntl12/14/17/31/34-38 (+_64)                      | perf, SIGIO, TCONF     |
| epoll_wait     |    2   | epoll_wait02 (timing), epoll_wait05 (connect TBROK)| timing, TCP loopback   |
| clock_*        |    4   | clock_gettime03/04, clock_nanosleep01-03           | NEWTIME NS             |
| clone          |    1   | clone09 (procfs TBROK)                             | -                      |
| bind/accept    |    2   | accept4_01, bind04                                 | AF_UNIX, TBROK         |
| cve            |    1   | cve-2017-17052 (fork+pthread+mmap Stress)          | memory-reclaim         |
| access         |    2   | access01, access04                                 | DAC edge-cases         |
| rest           |   12   | abort01, acct01, fchdir03, fchownat03, fdatasync02,| diverse                |
|                |        | dup05, dup201, fgetxattr02, fallocate02, stack_clash,|                      |
|                |        | copy_file_range03, epoll-ltp                       |                        |

## musl FAIL (11)

Stabil zu Baseline: fma, fmal, powf, remquol (softfma),
pthread_atfork-errno-clobber +static, rlimit-open-files +static,
pthread-robust-detach (flaky), tls_get_new-dtv, tls_init-static (flaky).

## Kernel-PFs

**Keine.** Nur "pages_alloc: order too large"-Warnung in fcntl34
(threads mit grossen Stacks) und SEGFAULT im bekannten tls_get_new-dtv.

## Priorisierung

**Top Fix-Kandidaten (naechste Phase):**

1. **fcntl17 TBROK** (2 tests) — Deadlock-Detection-Setup
2. **fcntl31 SIGIO** (2 tests) — sigtimedwait-Order
3. **epoll_wait02** (1 test) — hrtimer-Latenz bei 500×1ms
4. **clock_nanosleep01-03** (3 tests) — NEWTIME

**Deprioritized:** fcntl14/34/36 (perf), fcntl38 (dnotify si_fd),
access01/04 (DAC), cve-17052 (memory-reclaim).
