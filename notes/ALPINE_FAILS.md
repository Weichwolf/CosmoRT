# Alpine Test — Bestandsaufnahme

Run: 2026-04-21, nach hrtimer-UAF-Fix.

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Status          |
|-------|-------|------|------|------|-----------------|
| musl  | 478   | 461  |  10  |   7  | komplett durch  |
| LTP   | 313   | 139  | 124  |  35  | komplett durch  |

LTP läuft jetzt vollständig durch. Vorher Hang bei 67/313 (clock_nanosleep02)
wegen hrtimer-UAF, siehe fix.

## Root-Cause des früheren Hangs (gefixt)

LTP `clock_nanosleep01` SEND_SIGINT-Test timeoutet nach 3s (tst_test .timeout).
Framework SIGKILLt den Testprozess. Sein Thread blockte zu dem Zeitpunkt in
`do_nanosleep` → `thread_block_ms` mit einem stack-allozierten `hrtimer_t` im
globalen RB-Tree. `exit_kill_process` markiert den Thread DEAD und orphant ihn,
canceled den Timer aber nicht. Später `thread_free(t)` → `pages_free(t->kstack)`
gibt den Stack-Frame frei — der Timer im RB-Tree zeigt ins freigegebene Memory.
Nächster Timer-Fire: `t->fn(t)` lädt garbage-fn-ptr (oft 0, gerade
freier Slab), Kernel-PF `rip=0 cr2=0`.

Fix: `hrtimer_cancel_by_data(t)` in `thread_free` vor `pages_free`. Dequeued
alle Timer aus dem RB-Tree deren `data==t` ist. Kein dangling-pointer mehr.

## musl FAIL-Kategorien (10 unique, alle bekannt)

| Test                                   | Kategorie      | Ursache                                           |
|----------------------------------------|----------------|---------------------------------------------------|
| `fma`, `fmal`, `powf`, `remquol`       | math/FPU       | `qemu64` ohne CPU-FMA; musl softfma ULP-Drift + falsche fenv-Flags |
| `malloc-brk-fail-static`               | mm             | `malloc` gelingt nachdem brk OOM; ENOMEM nicht durchgereicht |
| `pthread_atfork-errno-clobber` +-static| rlimit         | `setrlimit(RLIMIT_NPROC,0)` nicht enforced bei fork |
| `rlimit-open-files` +-static           | rlimit         | `setrlimit` speichert `rlim_max` nicht            |
| `tls_get_new-dtv`                      | dl/tls         | dlopen mit dynamischer TLS-Slot-Allokation        |

## LTP Kernel-PFs (2 neue, non-fatal)

| Test       | PF-Addr              | rip (kernel)          | Hinweis                               |
|------------|----------------------|-----------------------|---------------------------------------|
| fcntl13    | cr2=0x7e20be0f8000   | 0xffff8000bcb4a3d5    | Prozess gekillt, LTP läuft weiter    |
| fcntl13_64 | cr2=0x7edef6585000   | 0xffff8000bcb4a3d5    | Gleicher rip wie fcntl13              |

Beide PFs nicht fatal — Prozess wird gekilled, LTP läuft weiter. Test FAILt
mit rc=2 (timeout). Vermutlich weiterer UAF oder race in fcntl13 (F_GETLK
mit range-lock hat vielleicht leaked-timer?).

## LTP FAIL-Gruppen

Unverändert aus voriger Bestandsaufnahme plus neue fcntl-Failures:

| Gruppe       | Beispiele                                  | Vermutung                       |
|--------------|--------------------------------------------|---------------------------------|
| chroot       | chroot01-04                                | chroot-Syscall -ENOSYS          |
| caps         | capget01, capset02-03                      | capabilities nicht implementiert |
| bpf          | bpf_prog02-04                              | bpf()-Syscall fehlt             |
| bind         | bind01-04                                  | AF_UNIX / edge-cases            |
| clock_gettime| clock_gettime01-04, clock_adjtime01-02     | CLOCK_TAI / adjtime             |
| chmod        | chmod05, chmod06                           | setuid bits                     |
| chown        | chown04                                    | chown + setuid-drop             |
| fcntl        | fcntl11/13/14/15/17/21/31/35/37            | file locking edge-cases         |

## Geänderte Dateien (dieser Run)

- `include/kernel/core/hrtimer.h` — `hrtimer_cancel_by_data` API
- `src/kernel/core/hrtimer.c` — impl (walk-leftmost-restart statt `rb_next`
  nach dequeue, da dequeue die Iteration invalidiert)
- `src/kernel/proc/process.c` — `thread_free` cancelt hrtimers vor kstack-free

## Nächste Fix-Kandidaten

1. **fcntl13 Kernel-PF (rip=0xffff8000bcb4a3d5)** — zweiter dangling-pointer-
   Pfad. Debug via ldbase + ELF-Offset.
2. **setrlimit rlim_max** — speichern, nicht verwerfen.
3. **brk-ENOMEM an malloc propagieren**.
4. **RLIMIT_NPROC fork-Enforcement**.

## Performance-Notiz

alpine-test komplett in ~30 Minuten. `timeout 1800` im Makefile reicht.
