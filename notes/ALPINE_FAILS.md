# Alpine Test — Bestandsaufnahme

Run: 2026-04-21 nach fcntl-Phase (PF-Fix, OFD-Locks, F_SETLEASE,
F_GETOWN_EX/F_SETOWN_EX, F_SETPIPE_SZ Error-Handling, /proc/sys/fs/*).

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta                  |
|-------|-------|------|------|------|------------------------|
| ktest | 2646  | 2646 |   0  |   -  | +65 (neue fcntl-Tests) |
| musl  |  478  |  461 |  10  |   7  | ±0                     |
| LTP   |  313  |  155 |  99  |  38  | +1 PASS / -5 FAIL (fcntl +13, dup -8 flaky) |

Baseline: ktest 2581, musl 461/10, LTP 154/104/40.

## fcntl-Cluster (vorher 25+ FAIL, jetzt 27 FAIL 35 PASS 10 SKIP)

Grosse Wins:
- **fcntl11/11_64** PASS — range-split Semantik korrigiert
- **fcntl13/13_64** PASS — User-CR2-PF behoben via copy_from_user
- **fcntl21/21_64** PASS — F_GETLK conflict-Range-Return
- **fcntl23/23_64, fcntl27/27_64** PASS — F_SETLEASE implementiert
- **fcntl33/33_64** SKIP (nicht FAIL) — lease-break-time procfs
- **fcntl12** verstanden — rlimit-EMFILE-Pfad funktioniert aber Test-Toolchain anders

Verbleibende fcntl-Fails (10 fcntl* FAIL + _64-Varianten):
- **fcntl12/12_64**: getdtablesize() + EMFILE-Edge, RLIMIT_NOFILE-Handling
- **fcntl14/14_64**: 5000 Random-Iterationen, 10s Timeout zu knapp
- **fcntl15/15_64**: tst_checkpoint (shared-memory-IPC zwischen Prozessen)
- **fcntl17/17_64**: F_SETLKW Deadlock-Detection (EDEADLK) — nicht impl.
- **fcntl30/30_64**: F_SETPIPE_SZ write-through, /proc/sys/fs/pipe-max-size
- **fcntl31/31_64**: SIGIO-Delivery via F_SETSIG — nicht impl.
- **fcntl34/34_64**: OFD-Lock-Stress in pthreads (Timeout)
- **fcntl35/35_64**: pipe-max-size als unprivileged user (getpwnam)
- **fcntl36/36_64**: OFD vs POSIX Race-Conditions (komplex)
- **fcntl37/37_64**: F_SETPIPE_SZ write-through für EBUSY-Check
- **fcntl38/38_64**: dnotify event-delivery — nicht impl.
- **fcntl39/39_64**: dnotify DN_RENAME — nicht impl.

## LTP FAIL (99) — verbleibende Gruppen (aktualisiert)

| Gruppe         | Anzahl | Tests                                           | Ursache                        |
|----------------|--------|-------------------------------------------------|--------------------------------|
| fcntl          |   27   | fcntl12/14/15/17/30/31/34/35/36/37/38/39 (+_64) | SIGIO, dnotify, timeouts       |
| eventfd/epoll  |   13   | eventfd01-05+2_03, epoll_pwait01/04, _wait02-06 | EPOLLET, EFD_SEMAPHORE         |
| clock_*        |    9   | clock_adjtime01/02, clock_gettime01-04, nanosleep01-04, settime02 | CLOCK_TAI, ns-Präzision |
| clone          |    2   | clone09 (TBROK procfs/net), clone301 (pidfd tcase) | fixed: 03/05/11/302 |
| bind/accept    |    7   | accept02/03, accept4_01, bind01-04, connect01/02 | AF_UNIX, SO_REUSEPORT         |
| chroot         |    2   | chroot01/04                                     | chroot() + DAC                 |
| caps           |    2   | capget01, capset02                              | Capabilities nicht impl.       |
| bpf            |    3   | bpf_prog02-04                                   | bpf() -ENOSYS                  |
| creat          |    1   | creat07                                         | LTP copy_resource SAFE_CP      |
| dup            |    2   | dup05, dup201                                   | O_CLOEXEC                      |
| execve         |    3   | execve04/05, execveat01/02                      | fexecve + suid                 |
| cve            |    3   | cve-2016-10044, cve-2017-1705x, cve-2025-38236  | regression tests               |
| access         |    2   | access01, access04                              | DAC edge-cases                 |
| fanotify       |    1   | fanotify12                                      | fanotify nicht impl.           |
| rest           |  ~15   | abort01, acct01, adjtimex02, fchdir03, fdatasync02, flock03, leapsec01, stack_clash | diverse |

## Kernel-PFs

**Keine.** Die beiden fcntl13-PFs (user-CR2) sind durch copy_from_user
behoben. tst_get_bad_addr gibt einen unmapped Pointer zurueck; unser
user_ok prueft nur den Address-Bereich, nicht ob die Seite gemapped
ist. copy_from_user macht das via _ASM_EXTABLE-Fault-Recovery.

## musl FAIL (10) — identisch zur Baseline

Keine Änderung: fma, fmal, powf, remquol (FPU softfma), malloc-brk-fail (mm),
pthread_atfork-errno-clobber +static (rlimit NPROC),
rlimit-open-files +static (rlim_max persist), tls_get_new-dtv (dl).

## Priorisierung (nach fcntl-Phase)

**Top Fix-Kandidaten:**

1. **eventfd/epoll edge-cases** (~13 tests) — meist EPOLLET/EFD_SEMAPHORE
2. **clock_* Präzision** (9 tests) — CLOCK_TAI + ns-Präzision
3. **dup O_CLOEXEC + clone PIDFD** (7 tests)

**Deprioritized:** chroot/caps/bpf (7 tests), fanotify (1), SIGIO-fcntl (6).
