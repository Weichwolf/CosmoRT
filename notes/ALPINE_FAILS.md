# Alpine Test — Bestandsaufnahme (Commit a144470)

Run: 2026-04-21, `make alpine-test` mit augmentiertem `tools/boot-test.sh`
(`set -x` + PROBE-Heartbeats). Kein Kernel-Hang — Tests liefen 25+ Minuten
kontinuierlich. Der Original-"Hang"-Eindruck kam vom stillen `find|sort|for`-
Loop ohne Output vor dem ersten PASS/FAIL.

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Status          |
|-------|-------|------|------|------|-----------------|
| musl  | 478   | 461  |  10  |   7  | komplett durch  |
| LTP   | 313   |  24  |  29  |   8  | Hang bei 67/313 |

LTP-Hang: `clock_nanosleep02` → Kernel-PF `pid=0x56 cr2=0 rip=0`
→ Test liefert FAIL, danach kommt `clock_nanosleep03` nicht mehr
(Timer-Subsystem vermutlich korrumpiert durch den PF).

## musl FAIL-Kategorien (10 unique)

| Test                                   | Kategorie      | Vermutete Ursache                               |
|----------------------------------------|----------------|-------------------------------------------------|
| `fma`, `fmal`, `powf`, `remquol`       | math/FPU       | `qemu64` hat kein CPU-FMA; softfma fallback hat ULP-Drift + falsche fenv-Flags (INEXACT/UNDERFLOW) |
| `malloc-brk-fail-static`               | mm             | `malloc(10000)` gelingt nachdem Memory voll; brk-ENOMEM nicht bis malloc durchgereicht |
| `pthread_atfork-errno-clobber` +-static| proc/rlimit    | `setrlimit(RLIMIT_NPROC,0)` + fork → fork erfolgt trotzdem |
| `rlimit-open-files` +-static           | proc/rlimit    | `getrlimit(RLIMIT_NOFILE)` liefert max=65536 nach `setrlimit(42,42)` — setrlimit speichert rlim_max nicht |
| `tls_get_new-dtv`                      | dl/tls         | dlopen-Pfad mit dynamischer TLS-Slot-Allokation |

## LTP FAIL-Liste (29)

```
abort01 accept02 access04 acct01 adjtimex02
bind01 bind02 bind03 bind04
bpf_prog02 bpf_prog03 bpf_prog04
capget01 capset02 capset03
chmod05 chmod06 chown04
chroot01 chroot02 chroot03 chroot04
clock_adjtime01 clock_adjtime02
clock_gettime01 clock_gettime02 clock_gettime03 clock_gettime04
clock_nanosleep01 clock_nanosleep02*
```

`*` clock_nanosleep02 endet mit Kernel-PF. Der PF ist benign behandelt
(`kill pid=...`), aber der LTP-Runner fährt danach nicht weiter.

## Gruppierung LTP-Fails

| Gruppe       | Tests                                                  | Vermutung                                    |
|--------------|--------------------------------------------------------|----------------------------------------------|
| chroot       | chroot01-04                                            | chroot-Syscall fehlt / -ENOSYS               |
| capget/caps  | capget01, capset02-03                                  | capabilities nicht implementiert             |
| bpf          | bpf_prog02-04                                          | bpf()-Syscall nicht implementiert            |
| bind         | bind01-04                                              | AF_UNIX / bind edge-cases                    |
| clock_gettime| clock_gettime01-04, clock_adjtime01-02                 | CLOCK_TAI / CLOCK_BOOTTIME / adjtime         |
| chmod        | chmod05, chmod06                                       | ownership / setuid bits                      |
| chown        | chown04                                                | chown-lutimes oder setuid-drop               |
| access       | access04                                               | faccessat edge-case                          |
| adjtimex     | adjtimex02                                             | adjtimex-Write-Mode                          |
| accept       | accept02                                               | accept error-returns                         |
| acct         | acct01                                                 | acct()-Syscall (deprecated)                  |
| abort        | abort01                                                | abort()-Verhalten / core-dump                |
| clock_nanosl | clock_nanosleep01-02                                   | absolute-time / CLOCK_PROCESS_CPUTIME        |

## Kritische Bugs (Prio-Reihenfolge)

1. **Kernel-PF in `clock_nanosleep02`** — blockiert komplettes LTP-Durchlaufen
   nach Test 67. Reproducer: `/opt/ltp/install/testcases/bin/clock_nanosleep02`.
   PID-Zahl 0x56 deutet auf LTP-Test-Child. `cr2=0 rip=0` = NULL-Jump.
2. **`setrlimit` ignoriert neue `rlim_max`** — blockiert jeden rlimit-Test.
3. **`brk`-ENOMEM wird nicht an malloc propagiert** — Ressource-Exhaustion-
   Semantik fehlt.
4. **`RLIMIT_NPROC` wird nicht enforced** bei fork — setrlimit speichert, aber
   fork-Pfad prüft nicht.
5. **FPU: fma/fmal/powf/remquol** — kein Kernel-Bug; qemu64 hat kein FMA
   und musl softfma setzt fenv-Flags falsch. Überspringen oder Host-baseline
   gegenchecken.

## Geänderte Dateien

- `tools/boot-test.sh` — `set -x` + PROBE-Heartbeats vor jedem musl-Test
  (bleiben drin: das Output-Rauschen kostet ~0.5s/Test, aber verhindert
  dass ein stiller Loop als Hang missgedeutet wird; zudem lassen sich
  Hänge jederzeit eindeutig verorten).

## Performance-Notiz

- musl-Durchlauf brauchte ~24 min für 478 Tests (~3s/Test).
- LTP-Durchlauf ~3 Tests/s, d.h. ~100 s für 313 ohne Hang.
- `make alpine-test` braucht total ~30 min; `timeout 1800` im Makefile reicht
  für musl allein, nicht für musl+LTP bis zum Ende. Hang bei 67/313 macht
  das aber gerade egal.

## Nächster Fix-Schritt (Empfehlung)

1. `clock_nanosleep02` lokal reproduzieren, Kernel-PF-Ursache finden.
   Vermutung: signal handler invoked mit NULL-Stack oder ucontext-Frame
   korrupt bei `SIGALRM`-Delivery während `clock_nanosleep` spinnt.
2. `setrlimit` fixen — Linux-Semantik: `rlim_max` darf nur gesenkt werden
   (nicht erhöht ohne CAP_SYS_RESOURCE). Im CosmoRT-Single-User-Kontext
   einfach speichern und zurückliefern.
3. `fork()`-Pfad muss `RLIMIT_NPROC` gegen aktuellen `nr_tasks_per_user`
   prüfen.
