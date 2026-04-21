# Alpine Test — Bestandsaufnahme

Run: 2026-04-21 nach VFS-Phase (EROFS mount-flag, ETXTBSY i_writecount,
mkdir-mode-Fix, tmpfs-ramfs-Overlay, fchmodat a4-Fix, LTPROOT env).

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta                  |
|-------|-------|------|------|------|------------------------|
| ktest | 2581  | 2581 |   0  |   -  | +22                    |
| musl  |  478  |  461 |  10  |   7  | ±0                     |
| LTP   |  313  |  154 | 104  |  40  | +16 PASS / -21 FAIL    |

Baseline: ktest 2559, musl 461/10, LTP 138/125/35.

Delta LTP: **+16 PASS** (chmod06, chown04, fchmod06, fchmodat01, fchmodat02,
fchown04, fchownat01, fchownat02, creat04, creat06, creat08 und weitere durch
mkdir-mode-Fix).

## LTP FAIL (104) — verbleibende Gruppen

| Gruppe         | Anzahl | Tests                                           | Ursache                        |
|----------------|--------|-------------------------------------------------|--------------------------------|
| fcntl          |  ~25   | fcntl11-39 (+_64), fcntl13 mit PF               | F_OFD_*, F_SETLEASE, UAF       |
| eventfd/epoll  |  ~13   | eventfd01-05+2_03, epoll_pwait01/04, _wait02-06 | EPOLLET, EFD_SEMAPHORE         |
| clock_*        |   9    | clock_adjtime01/02, clock_gettime01-04, clock_nanosleep01-04, clock_settime02 | CLOCK_TAI, ns-Präzision |
| clone          |   5    | clone03/05/09/11/301/302                        | CLONE_NEWNS, SETTID, PIDFD     |
| bind/accept    |   7    | accept02/03, accept4_01, bind01-04, connect01/02 | AF_UNIX, SO_REUSEPORT         |
| chroot         |   1    | chroot01                                        | chroot() + DAC                 |
| caps           |   2    | capget01, capset02                              | Capabilities nicht impl.       |
| bpf            |   3    | bpf_prog02-04                                   | bpf() -ENOSYS                  |
| creat          |   1    | creat07                                         | LTP copy_resource SAFE_CP       |
| dup            |   2    | dup05, dup201                                   | O_CLOEXEC                      |
| execve         |   3    | execve04/05, execveat01/02                      | fexecve + suid                 |
| cve            |   3    | cve-2016-10044, cve-2017-1705x, cve-2025-38236  | regression tests               |
| access         |   2    | access01, access04                              | DAC edge-cases                 |
| fchownat       |   1    | fchownat03                                      | 1 subtest (EACCES/EPERM mismatch?) |
| fanotify       |   1    | fanotify12                                      | fanotify nicht impl.           |
| rest           |  ~15   | abort01, acct01, adjtimex02, faccessat202, fallocate02, fchdir03, fdatasync02, fgetxattr02, flock03, leapsec01, posix_fadvise02_64, stack_clash | diverse |

## Kernel-PFs (unverändert 2)

| Test       | rip (kernel)          |
|------------|-----------------------|
| fcntl13    | 0xffff8000...         |
| fcntl13_64 | gleiche Stelle        |

Nicht fatal, Prozess gekilled, LTP läuft weiter.

## musl FAIL (10) — identisch zur Baseline

Keine Änderung durch VFS-Phase:
fma, fmal, powf, remquol (FPU softfma), malloc-brk-fail (mm),
pthread_atfork-errno-clobber +static (rlimit NPROC),
rlimit-open-files +static (rlim_max persist), tls_get_new-dtv (dl).

## Priorisierung (nach VFS-Phase)

**Top Fix-Kandidaten:**

1. **fcntl F_OFD_*/F_SETLEASE + PF-Fix** (~25 tests, mittel)
2. **eventfd/epoll edge-cases** (~13 tests, niedrig) — meist rc=2 (timeout)
3. **clock_* Präzision** (9 tests, mittel) — CLOCK_TAI nicht impl, tst_test clock-Drift

**Deprioritized:** chroot/caps/bpf (6 tests), fanotify (1 test).
