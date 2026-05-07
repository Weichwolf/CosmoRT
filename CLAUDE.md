# CosmoRT

Linux/POSIX/musl-ABI-kompatibler Kernel für moderne Hardware. Kein Legacy. Stabil, schlank, performant.

**Targets:** x86_64 · aarch64 | **Userland:** Alpine (musl libc)

---

## 1. Architektur

```
Layer 5  Userspace ............ musl · apk · sshd · bash
Layer 4  POSIX-Subsysteme ..... VFS · net · IPC · signals
Layer 3  Core-Kernel .......... sched · MM · syscall-dispatch
Layer 2  HAL .................. irq · timer · cpu · mmu
Layer 1  Platform/BSP ......... boot · SMP-init
Layer 0  Hardware ............. CPU · MMU · APIC/GIC · timer
```

Regeln:
- Abhängigkeiten nur abwärts. Kein Layer kennt seinen Aufrufer.
- HAL ist die einzige Plattformgrenze. Darüber: arch-agnostisch.
- Subsysteme kommunizieren über Core-Primitiven, nie lateral.
- Verzeichnisstruktur und Header sind authoritativ — nicht hier dokumentieren.

---

## 2. POSIX-Compliance: was zählt

musl-libc-Pflicht:

| Gruppe   | Syscalls                                                       | Kritisch für           |
|----------|----------------------------------------------------------------|------------------------|
| File-I/O | read, write, open(at), close, lseek, stat, fstat, ioctl       | apk, alle Tools        |
| Process  | clone, execve, exit, wait4, getpid, getppid                   | init, sh               |
| Memory   | mmap, munmap, mprotect, brk                                   | malloc                 |
| Signals  | rt_sigaction, kill, sigprocmask, rt_sigreturn                 | Error-Handling         |
| Time     | clock_gettime, nanosleep, clock_nanosleep                     | TLS, Timeouts          |
| IPC      | pipe2, socketpair, **futex**                                  | pthreads (über futex!) |
| Network  | socket, bind, connect, accept, sendmsg, recvmsg               | sshd, apk fetch        |
| FS       | getcwd, chdir, mkdirat, unlinkat, renameat, getdents64        | apk, busybox           |
| TTY      | ioctl(TIOCGWINSZ), tcsetattr                                  | interaktive Shell      |

`futex` ist nicht optional. Ohne korrekte FUTEX_WAIT/WAKE-Semantik hängen alle Threads.

---

## 3. Bewusste Auslassungen

| Feature              | Grund                                                |
|----------------------|------------------------------------------------------|
| Multi-User / DAC/MAC | Single-User, kein POSIX-uid-Enforcement              |
| Kernel-Module (.ko)  | Komplexität ohne Nutzen                              |
| IOMMU/DMA-Remapping  | Nur multi-tenant relevant                            |
| Swap                 | OOM-kill statt Paging-IO                             |
| Audit/seccomp        | Optional via syscall_dispatch-Hook nachrüstbar       |
| Legacy-Syscalls      | Delegieren an moderne *at-Varianten (Einzeiler)      |

---

## 4. Build

`make` immer VOR Test-Targets — Makefile trackt keine Header-Deps; Struct-Änderung sonst stale .o.

```sh
make                    # → build/BOOTX64.EFI
make test-hw            # ktest in QEMU
make test-crash         # Adversarial
make test-fuzz          # Syscall-Fuzzer
make alpine-test        # Boot Alpine → musl + LTP, fail-stop
make qemu-alpine        # Normaler Boot (OpenRC/getty)
```

KVM ist Pflicht (`-machine accel=kvm:tcg`). TCG nur Fallback. User in `kvm`-Gruppe (`sudo usermod -aG kvm $USER`). `LTP_TIMEOUT_MUL=1` — Multiplier maskiert Wurzeln.

Fokussierter Iter-Run (~3 min):
```sh
echo 'test1|test2' > build/alpine-root/opt/musl_run    # Filter
echo '__none__'    > build/alpine-root/opt/ltp_run     # LTP aus
make alpine-test
rm build/alpine-root/opt/{musl_run,ltp_run}
```
Marker gesetzt → `DEBUG=1` (voller Output statt Head).

Serial-Log live: `/tmp/cosmo-serial.log` (test-hw) bzw. `/tmp/alpine-test.log`.

---

## 5. C-Standard

`gnu11`, freestanding, `-Werror`. GNU-as für `.S`, kein NASM.

- **`_Static_assert`** auf jeden ABI-/ASM-Kontrakt und jede Hardware-Struct-Größe. Kompile-Zeit-Check ist die einzige Zeile, die im Kernel nicht silent korrumpiert.
- **`__typeof__`** für jedes Makro, das einen Ausdruck mehrfach auswertet. `container_of` ist Pflicht.
- **`__attribute__((warn_unused_result))`** auf alles, was fehlschlagen kann (Alloc, Map, Lock-Acquire, Init). `(void)`-Cast nur mit dokumentierter Begründung.
- Kein RAII, keine Exceptions, kein Sanitizer im Hot-Path. Compiler ist letzte Verteidigung — gib ihm Information.

---

## 6. Ressourcen-Regel: keine fixen Pools

Systemweite Pools sind verboten. Ein Prozess, der alles aufbraucht, ist ein Angriffsvektor, kein Limit.

Jede prozess-allozierte Ressource:
1. **Slab-allokiert** — kein statisches Array
2. **Per-Prozess gecapped** via RLIMIT, nicht global
3. **On-demand wachsend** — nichts vorab reservieren

Ausnahme nur, wo Hardware (IDT/IRQ: 256) oder POSIX (Signals: 64) es erzwingt.

`#define FOO_MAX <n>` gefolgt von `foo_t pool[FOO_MAX]` ist Review-Blocker.

Reihenfolge bei Neuentwicklung: Slab-Cache → RLIMIT → `_Static_assert` → Funktionalität.

---

## 7. Default: implementiere wie Linux

Abweichungen nur mit Begründung im Commit.

| Regel                                                                | vs Linux  | CosmoRT-Abweichung                                |
|----------------------------------------------------------------------|-----------|---------------------------------------------------|
| Stack-Ownership: ein Thread, ein Kernel-Stack, exklusiv              | gleich    | —                                                 |
| `context_switch(prev,next)`: ein Mechanismus, ein Callsite, atomar   | gleich    | `ret_from_fork` als Resume-Target ist inhärent    |
| State-Change und Switch atomar in einer Operation                    | gleich    | —                                                 |
| Ownership: ein Owner oder expliziter Refcount, nie implizit shared   | gleich    | —                                                 |
| Ein Pfad pro Konzept: fork/vfork/clone → eine Implementierung        | strenger  | kein Legacy — eine Funktion mit Flags             |
| Bounded Execution: Core-Pfade bounded, I/O timeout-guarded           | strenger  | Linux erlaubt unbounded Paths                     |
| Fail-Stop: Fehler → Panic oder `-ERRNO`, nie stille Korruption       | strenger  | Linux: WARN_ON + Recovery                         |
| Explizite Dependencies: Jede Abhängigkeit im Typ/API sichtbar        | strenger  | kein initcall-Level-Magic                         |
| Interrupt-Transparenz: Jeder Punkt unterbrechbar oder geschützt      | gleich    | konsistent mit PREEMPT_RT                         |
| Zero-Copy wo möglich: Pointer statt Daten                            | gleich    | splice, sendfile                                  |
| Idempotenz: Syscall-Restart safe, doppeltes Wake = No-Op             | gleich    | —                                                 |

---

## 8. Code-Regeln

**ABI:**
- `linux.h` ist exakt Linux x86_64 ABI. Keine Abweichungen.
- Jedes `linux.h`-Define muss implementiert UND getestet sein.
- Keine Magic Numbers — benannte Konstanten aus `linux.h`.
- Keine Stubs (`return 0`). Korrekt implementieren oder `-ENOSYS`.

**Organisation:**
- Bottom-Up: Helpers oben, Caller unten. Keine Forward-Declarations.
- Hot-Path frei von Strings/Error-Output. `__attribute__((cold))` auslagern.
- `__attribute__((hot))` auf Syscall-Dispatch, `__attribute__((cold))` auf Panic/Init.
- `__builtin_expect` auf Fehlerpfade im Hot-Path.

**Portabilität:**
- `src/kernel/` hat kein inline-asm — alles via `arch_*()` in `src/arch/`.
- `src/arch/{x86_64,aarch64}/` — keine weitere Arch.
- Treiber: nur `include/public/`, nie Kernel-Interna.

---

## 9. Tests

- Jeder neue Syscall/Fix bekommt Test.
- Jede Änderung automatisch testbar.
- `TEST()` für Unit, `CRASH_TEST()` für Adversarial.
- Self-Registering via Linker-Section. `main.c` nicht anfassen.
- Test darf nie hängen — blockiert er, ist es ein Kernel-Bug.
- `make test-hw` muss grün sein vor Merge.

### Bug-Workflow: alpine-test → test-hw → Fix

Bug in `alpine-test` (musl/LTP) ist erst gefixt, wenn die Wurzel in `test-hw` reproduziert ist. Pflichtreihenfolge:

1. Alpine-Test failt → Symptom + Annahme über Wurzel notieren
2. **Reproduktion in `test-hw`** als `TEST()` schreiben — der Test muss vor dem Fix rot sein
3. Fix implementieren
4. `make test-hw` grün
5. `make alpine-test` grün — der ursprüngliche Alpine-Test muss passen
6. Regression in irgendeinem anderen Test ist nie akzeptabel — kein Trade

Begründung: alpine-test ist langsam (~30–45 min), schwer zu debuggen, deckt nur das Symptom. test-hw ist schnell, deterministisch, isoliert die Wurzel. Ohne test-hw-Repro fehlt der Regressionsschutz; der gleiche Bug kommt zurück, nur unter neuem Stack.

Ausnahme nur, wenn Bug nachweislich Userspace-Tooling-Schicht (busybox vs GNU, musl-Bug) — dann kein Kernel-Fix, alpine-test-Erwartung als TFAIL akzeptiert dokumentieren.

---

## 10. TODO.md

- Nach jedem Commit prüfen.
- Erledigtes sofort abhaken oder komprimieren.
- Testcount im Header aktualisieren.
- Alte Phasen in Zusammenfassung verschieben.
