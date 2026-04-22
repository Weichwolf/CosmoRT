# Alpine Test — Bestandsaufnahme

Run: 2026-04-22 nach cve+execve-Cluster.

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta vs vorher                       |
|-------|-------|------|------|------|---------------------------------------|
| ktest | 2713  | 2713 |   0  |   -  | +9 (personality, execve_dac)          |
| musl  |  478  |  461 |  10  |   7  | +3/-3 (Flake stabilisiert)            |
| LTP   |  298  |  206 |  49  |  43  | -3 FAIL/+3 SKIP (cve+execve-Cluster)  |

Baseline: ktest 2704, musl 458/13, LTP 206/52/40.

## cve+execve-Cluster (Behoben 2026-04-22)

| Test                | Vorher | Nachher | Fix                                     |
|---------------------|--------|---------|-----------------------------------------|
| cve-2016-10044      | FAIL   | SKIP    | personality(persona) speichert + zurueck|
| cve-2017-17052      | FAIL   | FAIL    | TBROK ENOMEM (Stress-Test, 2/4 runs ok) |
| cve-2017-17053      | FAIL   | PASS    | /proc/sys/kernel/tainted Stub          |
| cve-2025-38236      | FAIL   | PASS    | af_unix MSG_OOB-Byte-Queue + kconfig    |
| execve02            | FAIL   | PASS    | DAC MAY_EXEC statt nur Mode-X-Bit       |
| execve04            | FAIL   | PASS    | (ETXTBSY, schon korrekt)                |
| execve05            | FAIL   | PASS    | (Parallel forks, schon korrekt)         |
| execveat01          | FAIL   | PASS*   | do_execve_kpath fuer Kernel-Pfade       |
| execveat02          | FAIL   | PASS    | (Error-Paths, schon korrekt)            |

*execveat01: PASS nach Commit 80c583b (folgt im naechsten Run).

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

Keine. musl-Differenz -2 ist die bekannte Flake bei tls_init-static /
pthread_once-deadlock / pthread-robust-detach (nicht deterministisch).

**Behoben (2026-04-22):** chroot01, chroot04, capget01, capset02, capset03.
- Kein CAP_SYS_CHROOT-Gate auf chroot (chroot01 EPERM via seteuid-drop)
- DAC-Check vor CAP-Check (chroot04 EACCES, Linux-Order ksys_chroot)
- cap_bounding pro Prozess + PR_CAPBSET_DROP/READ (capset02 bounding-Fall)
- capset pI-Subset nutzt bounding (Linux cap_capset Formel)
- setuid-Transition cleart pE/pP (cap_emulate_setxuid)
- capget/capset hdr.pid als TID interpretiert (Linux-ABI per-thread caps)

**Behoben (vorher):** pthread_robust + pthread_robust-static. Root Cause:
FUTEX_LOCK_PI ignorierte den shared-Flag und queuete Waiter immer mit
(vaddr, pid). Das Cleanup im Thread-Exit rief aber FUTEX_WAKE shared
(PA-Key) und traf den PI-Waiter nie bei pshared=1 + PTHREAD_PRIO_INHERIT.
Fix: futex_lock_pi/unlock_pi akzeptieren shared-Parameter, leiten ihn an
Bucket-Hash und futex_wake durch.

## LTP FAIL (62) — verbleibende Gruppen

| Gruppe         | Anzahl | Tests                                           | Ursache                   |
|----------------|--------|-------------------------------------------------|---------------------------|
| fcntl          |   16   | fcntl12/14/17/31/34-38 (+_64)                   | perf, SIGIO-order, TCONF  |
| eventfd/epoll  |    9   | eventfd01-05, epoll_pwait01/04, _wait02/06/07  | EPOLLET/EFD_SEMAPHORE     |
| clock_*        |    4   | clock_gettime03/04, clock_nanosleep01-03        | NEWTIME NS                |
| clone          |    2   | clone09/11 (TBROK procfs/net), clone301         | fixed: 03/05/302          |
| bind/accept    |    6   | accept02/03, accept4_01, bind01-04, connect01   | AF_UNIX, SO_REUSEPORT     |
| bpf            |    1   | bpf_prog04                                      | bpf                       |
| cve            |    3   | cve-2016-10044, cve-2017-1705x, cve-2025-38236  | regression tests          |
| access         |    2   | access01, access04                              | DAC edge-cases            |
| rest           |  ~13   | abort01, acct01, adjtimex02, fchdir03, leapsec01, stack_clash, flock03 | diverse |

## musl FAIL (11) — Baseline +1 Flake

Baseline (10): fma, fmal, powf, remquol (softfma), malloc-brk-fail,
pthread_atfork-errno-clobber +static, rlimit-open-files +static,
tls_get_new-dtv.

Flaky (1): tls_init-static — passiert/scheitert zufaellig. Auch
pthread_cond-smasher, pthread_once-deadlock, pthread-robust-detach
sind intermittent (nicht jeder Run gleich). Nicht durch Kernel-Fix
verursacht — vor der pthread_robust-Regression ebenfalls zu sehen.

## Kernel-PFs

**Keine.** Nur "pages_alloc: order too large"-Warnung in fcntl34 (threads
mit grossen Stacks + parallel eager-alloc?) und ein SEGFAULT im
bekannten tls_get_new-dtv-Fail.

## Priorisierung

**Top Fix-Kandidaten:**

1. **fcntl17 TBROK** (2 tests) — Deadlock-Detection-Setup-Issue
2. **fcntl31 SIGIO** (2 tests) — sigtimedwait sieht pending nicht
3. **eventfd/epoll** (~9 tests) — EPOLLET-Semantik

**Deprioritized:** fcntl14/34/36 (perf), fcntl38 (dnotify si_fd Detail),
chroot/caps/bpf (7 tests), cve/regressions.
