# Alpine Test — Bestandsaufnahme

Run: 2026-04-21 nach Phase 7.7 (Timer-Treiber: HPET-Kalibrierung, TSC-Invariant-
Check, KVM pvclock, Hyper-V STimer0, ACPI PM_TMR, virtio-rtc probe).

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta                  |
|-------|-------|------|------|------|------------------------|
| ktest | 2559  | 2559 |   0  |   -  | ±0                     |
| musl  |  478  |  461 |  10  |   7  | ±0                     |
| LTP   |  313  |  138 | 125  |  35  | -1 PASS / +1 FAIL      |

Baseline vor Phase 7.7: musl 461/10, LTP 139/124/35.

Delta LTP: **+1 FAIL, -1 PASS** — ein Test der vorher grün war ist jetzt rot.
Identität nicht aus Baseline rekonstruierbar (Baseline hatte keine namentliche
Liste). Kandidaten sind in der fcntl-Gruppe (größte Gruppe, größte Oberflächen-
änderung durch neue Timer-Quellen — hrtimer-Affekte).

## Kernel-PFs (unverändert 2, rip verschoben durch Treiber-Linkage)

| Test       | PF-Addr              | rip (kernel)          |
|------------|----------------------|-----------------------|
| fcntl13    | cr2=0x00007ec7f4eb3000 | 0xffff8000bcb39695  |
| fcntl13_64 | cr2=0x00007ecd46f58000 | 0xffff8000bcb39695  |

Nicht fatal, Prozess gekilled, LTP läuft weiter. Gleicher rip wie vor Phase 7.7
(0xffff8000bcb4a3d5 → 0xffff8000bcb39695; Offset ist Code-Layout, nicht neue
Bug-Site).

## musl FAIL (10) — identisch zur Baseline

| Test                                   | Kategorie  | Ursache                                    |
|----------------------------------------|------------|--------------------------------------------|
| fma, fmal, powf, remquol               | math/FPU   | qemu64 softfma ULP-Drift + fenv-Flags      |
| malloc-brk-fail-static                 | mm         | malloc gelingt nach brk OOM                |
| pthread_atfork-errno-clobber (+static) | rlimit     | setrlimit(RLIMIT_NPROC,0) fork-Enforce     |
| rlimit-open-files (+static)            | rlimit     | setrlimit speichert rlim_max nicht         |
| tls_get_new-dtv                        | dl/tls     | dlopen dyn TLS-Slot                        |

## LTP FAIL (125) — Gruppierung

| Gruppe         | Anzahl | Tests                                           | Ursache-Vermutung              |
|----------------|--------|-------------------------------------------------|--------------------------------|
| fcntl          |  36    | fcntl11-39 (+_64), fcntl13 mit PF               | File-Locking, F_OFD_*, leases  |
| eventfd/epoll  |  13    | eventfd01-05+2_03, epoll_pwait01/04, _wait02-06, epoll-ltp | Event-FD + EPOLL edge-cases    |
| clock_*        |   9    | clock_adjtime01-02, clock_gettime01-04, clock_nanosleep01-03, clock_settime02 | CLOCK_TAI, adjtime, ns-Präzision |
| clone          |   6    | clone03/05/09/11/301/302                        | CLONE_NEWNS, SETTID, PIDFD     |
| chmod/chown    |   7    | chmod05/06, chown04, fchmod05/06, fchmodat01/02, fchown04, fchownat02/03 | setuid-bits, CAP_CHOWN         |
| bind/accept    |   7    | accept02/03, accept4_01, bind01-04, connect01/02 | AF_UNIX, SO_REUSEPORT          |
| chroot         |   4    | chroot01-04                                     | chroot() -ENOSYS               |
| caps           |   3    | capget01, capset02-03                           | capabilities nicht impl.       |
| bpf            |   3    | bpf_prog02-04                                   | bpf() -ENOSYS                  |
| creat          |   4    | creat04/06/07/08                                | O_CREAT + setuid + perms       |
| dup            |   3    | dup05, dup201, dup3_01                          | O_CLOEXEC flag handling        |
| execve         |   5    | execve02/04/05, execveat01/02                   | suid exec + fexecve            |
| cve            |   4    | cve-2016-10044, cve-2017-1705x, cve-2025-38236  | regression tests               |
| access         |   2    | access01, access04                              | EACCESS vs EROFS               |
| fanotify       |   1    | fanotify12                                      | fanotify nicht impl.           |
| rest           |  12    | abort01, acct01, adjtimex02, faccessat202, fallocate02, fchdir03, fdatasync02, fgetxattr02, flock03, leapsec01, posix_fadvise02_64, stack_clash | diverse                        |

Hinweis: fcntl33(_64), fcntl35(_64) haben rc=36 (LTP TCONF), werden aber von
`tools/boot-test.sh` als FAIL gezählt. Korrekt als SKIP gezählt wäre LTP 138/121/39.

## Priorisierung

Yield = Anzahl Tests die ein Fix grün macht. Aufwand = Implementierungs-Tiefe.

**Top 3 Fix-Kandidaten:**

1. **fcntl F_OFD_*/F_SETLEASE + PF-Fix** (Yield ~30 tests, Aufwand mittel)
   - fcntl13 PF rip=0xffff8000bcb39695 weiterhin offen (zweiter UAF-Pfad)
   - F_OFD_SETLK, F_OFD_GETLK, F_OFD_SETLKW (offene Datei-Descriptor Locks)
   - F_SETLEASE, F_GETLEASE
   - Lock-Range-Konflikt-Detektion
   - Größter single-group yield.

2. **eventfd/epoll edge-cases** (Yield ~13 tests, Aufwand niedrig)
   - Meist rc=2 (timeout) — vermutlich Semantik-Abweichungen bei
     EPOLLET, EFD_NONBLOCK, EFD_SEMAPHORE.
   - Konkrete Fehlermuster pro Test auswertbar durch LTP-output-Capture.

3. **setuid-bit preservation in chmod/chown/creat** (Yield ~11 tests, Aufwand niedrig)
   - chmod05/06, chown04, fchmod05/06, fchmodat01/02, fchown04,
     fchownat02/03, creat04/06/08.
   - Implementierung: bei chmod S_ISUID/S_ISGID korrekt setzen, bei chown
     nach Regeln droppen (POSIX.1-2008 §6.4.2).

**Explizit deprioritized:**

- **chroot/caps/bpf** (10 tests): große Implementierungen, niedriger Single-User-
  Nutzen. chroot() ist implementierbar (~200 LoC), caps und bpf sind weit.
- **clock_* (9 tests)**: trotz Phase 7.7 haben sich keine neu gelöst. rc=2
  (timeout) deutet auf Framework-Probleme (tst_test clock_gettime-Präzision,
  nicht Kernel-Bug). Niedrige Yield-Dichte.
- **fcntl33/35 rc=36**: Messfehler in `tools/boot-test.sh` — rc=36 sollte als
  SKIP gezählt werden (LTP TCONF). 4 schnelle "Fails" per Test-Runner-Fix.

## Empfehlung Reihenfolge

1. boot-test.sh: rc=36 → SKIP (+4 Fails weg ohne Kernel-Arbeit)
2. setuid-bit group (chmod/chown/creat) — einfach, POSIX-well-defined
3. fcntl13 PF-Debug — gleicher rip wie Baseline, Debug-Pfad schon bekannt
4. eventfd/epoll edge-cases
5. fcntl locking advanced (F_OFD_*, F_SETLEASE)
