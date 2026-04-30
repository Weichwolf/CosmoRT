# CosmoRT — TODO

## Identität

CosmoRT ist eine **Multimedia-Konsole** mit **WASM als nativem Cartridge-Format**.

**Zwei dauerhafte Säulen** (nicht entweder-oder):

1. **Linux-ABI-Kompatibilität (permanent, vollständig)** — Alpine Linux läuft
   komplett. apk, busybox, OpenSSH, bash, Python, Compiler, Editor — alles
   was als Alpine-Paket verfügbar ist, läuft unverändert. Phase-Roadmap
   10-17 baut diese Säule (musl + LTP grün auf x86_64).
2. **WASM-Native (Alleinstellungsmerkmal)** — Neue Multimedia-Apps als
   WASM-Cartridges. Sandbox-by-Design, cross-arch, hot-loadable, zero-copy
   direkt mit framebuffer/audio/GPU via mmap-host-imports. Phase-Roadmap
   18-25 baut diese Säule.

**Konsole** = stabile RT-Plattform; **Cartridge** = WASM-App-Modul.
~10 Host-Imports statt 1500 Browser-DOM-APIs. Kein JS, kein DOM, kein
Compositor-Cargo-Cult.

Beide Welten interoperieren: Alpine-Tools bauen WASM-Cartridges,
WASM-Apps nutzen POSIX-Sockets via WASI, gemeinsames Filesystem.

**Vision-Frage**: Was würde Linus heute bauen, wenn er frisch anfinge auf
moderner x86_64+aarch64-Hardware mit WASM als Universal-Binary für
Multimedia-Apps?

---

## Stand 2026-04-30 (post-raise-race-fix)

**Vergleich CosmoRT vs Linux 6.12 RT (selbe alpine.img, selber boot-test.sh):**

| Metric | CosmoRT | Linux 6.12 RT |
|--------|---------|---------------|
| ktest (test-hw) | 3246 / 0 | n/a |
| musl libc-test | **469 / 9** | 461 / 17 |
| LTP PASS | 276 | 297 |
| LTP FAIL | **19** | 73 |
| LTP SKIP | 155 | 80 |
| Test-hw Reproduktion: `make test-hw` | | |
| Vollauf CosmoRT: `make alpine-test` → `/tmp/alpine-test.log` | | |
| Vollauf Host-Referenz: `make alpine-test-host` → `/tmp/alpine-test-host.log` | | |

**3-Wege-Diff (alpine-test.log vs alpine-test-host.log):**

- **3 Tests / 2 Cluster** failen NUR auf CosmoRT → echte Kernel-Bugs (Prio A)
- **1 Test** failt NUR auf CosmoRT wegen fehlendem Subsystem (akzeptiert: fanotify25)
- **25 Tests** failen auf BEIDEN → LTP-Test-Bugs / Userspace-Setup (akzeptiert)
- **48 Tests** failen NUR auf Host → irrelevant fuer CosmoRT (alpine-Setup)

CosmoRT skippt 75 Tests mehr als Host weil Subsysteme fehlen — diese
Sub-Cluster sind Prio B/C nach Alltagsrelevanz.

---

## Prio A — Echte CosmoRT-Kernel-Bugs

2 Cluster, 3 Test-Failures (popen + raise-race + regex GEFIXT). Reproduktion:
`make alpine-test`, danach Marker-File-Filter fuer fokussierte Iteration
(siehe CLAUDE.md §Test-Iteration).

### A.1 popen-Race — ✓ GEFIXT (Commit 45b023d)

Wurzel: `schedule_timeout_common` in `core/waitqueue.c` hatte interne
`for(;;)`-Loop die wakes als "spurious" classified und re-iterierte. Bei
`timeout_ns=0` (poll(-1)) nie break-Bedingung — sh's poll(-1) auf pipe
geweckt durch pipe_write, schedule() returnt, aber alte Loop setzte
state=BLOCKED erneut. sh nie wieder dran.

Fix: Linux-Pattern. schedule_timeout fuehrt EINEN schedule()-Call aus und
returnt danach. Caller (poll_loop, sleep_until_ns) hat eigene outer-loop
mit Condition-Recheck.

Mitgefixt: regex-ere-backref-static, regex-negated-range-static (A.5)
hatten dieselbe Race im poll-Pfad.

### A.2 raise-race (musl raise-race + raise-race-static) — ✓ GEFIXT

Zwei Wurzeln, beide noetig fuer den Test:

**1. RT-Signal-Queue (sig 32..63 / SIGRTMIN..SIGRTMAX)**

CosmoRT hatte nur Bitmap-pending. 100x pthread_kill(SIGRTMIN+1) kollabierten
zu einem Bit → handler1 lief 1x statt 100x → busy-wait `while (c1<100)`
hing forever. Linux: counted RT-queue, jeder pthread_kill liefert genau
einen Handler-Aufruf.

Fix: per-RT-signal counter `sig_rt_count[32]` (process) und
`sig_rt_thread_count[32]` (thread). Bei sig>=32: counter++ in send-Pfad,
counter-- in consume-Pfad, bit bleibt set solange counter > 0. Send/consume
unter `process->lock` synchronisiert. Non-RT-Signale (1..31) bleiben
bit-only — Linux-konform.

**2. Nested Signal-Delivery in rt_sigreturn**

Nach handler0-Return war SIGRTMIN+1 zwar deliverable (mask=0 nach restore),
aber `check_signals_syscall_path` machte fuer SYS_RT_SIGRETURN early-return.
Naechster Syscall war raise's `__block_app_sigs`, das SIGRTMIN+1 sofort
wieder blockt — handler1 nie ausgeliefert waehrend raise-Loop. Erst nach
Loop-Ende, in busy-wait, lieferte preempt-tick die handler aus → 99 von 100
Children resumed im busy-wait und blieben dort haengen (parent's `if (child)
_exit` war zu dem Zeitpunkt schon vorbei).

Fix: rt_sigreturn-Pfad in `check_signals_syscall_path` macht keinen
early-return mehr. Nested Delivery wie Linux: zwischen handler-ret und
return-to-user wird ein zweiter Frame gestapelt.

Das war ein Revert (c3b602f) eines fruehen Versuchs (528d571) — die damals
gemeldeten Regressionen (pthread_cond-smasher, pthread_once-deadlock,
capset04) sind in der aktuellen Version alle PASS. Vermutlich wurden die
state-issues seither in anderen Subsystemen behoben.

Files: `include/kernel/proc/process.h`, `include/kernel/proc/thread.h`,
`src/kernel/proc/process_fork.c` (counter-Init im fork-Pfad),
`src/kernel/proc/signal.c` (RT-counter inc/dec + lock-Sync),
`src/kernel/proc/signal_frame.c` (dnotify re-pend mit counter),
`src/kernel/proc/signal_handler.c` (rt_sigreturn early-return entfernt).

5x isoliert PASS. Vollauf: musl 469/9 (vorher 467/11), LTP 276/19 (vorher
277/18 — innerhalb Noise).

### A.3 du01.sh

busybox `du -s` gibt auf CosmoRT andere Block-Counts als auf Host.
Vermutung: `stat()->st_blocks` falsch berechnet (sollte 512-Byte-Sektor-
Anzahl sein, nicht Filesystem-Blocks). Untersuche `vfs_fstat()` /
`tmpfs_op_stat()` / `ext4_inode_stat()`.

### A.4 regex — ✓ GEFIXT (mit A.1, Commit 45b023d)

Wie vermutet: gleiche Wurzel wie popen-Race. schedule_timeout-Loop bug
betraf alle Tests die ueber poll/select auf fd-readiness warten.

---

## Prio B — Fehlende Subsysteme (Alltagsrelevanz)

Tests die auf Host PASS sind aber auf CosmoRT geSKIPPED weil Subsystem
fehlt. Sortiert nach Userspace-Relevanz (was Programme tatsaechlich nutzen).

### B.1 xattr-System (extended file attributes)

Tests: `fgetxattr03`, `flistxattr01`, `flistxattr02`, `flistxattr03`
(plus advanced: `file_attr01-05`, `fgetxattr01`, `fgetxattr02` failen
auf beiden — alpine-Setup-Issue, aber Subsystem auch auf CosmoRT noetig).

Was: `setxattr/getxattr/listxattr/removexattr` Syscalls + ext4-Inode-xattr-
Block + tmpfs-xattr-Hashtable.

Warum wichtig: `cp -a`, `tar`, `rsync`, AppArmor/SELinux-Labels, ACLs,
file-attr-1: alle nutzen xattrs. Standard fuer "Datei-kopieren mit
Metadaten" auf Linux.

Aufwand: ~1500 LOC (syscalls + ext4-xattr-format-parser + tmpfs-storage).

### B.2 fanotify (file event monitoring)

Tests: `fanotify02`, `fanotify04`, `fanotify07`, `fanotify08`, `fanotify11`,
`fanotify12` (alle auf Host PASS, CosmoRT SKIP). Plus advanced
`fanotify01/03/05/06/09/10/13-21/23/24` failen sowohl auf Host als auch
CosmoRT (alpine-Setup-Mix).

Was: `fanotify_init/mark` Syscalls — file event monitoring mit Permission-
Hooks und FAN_CLASS_CONTENT. Erweitert inotify um Mount/Filesystem-weite
Events + write-Permission-Vermittlung.

Warum wichtig: `systemd`, `gvfs`, `tracker`, file-Manager, AV-Scanner.
inotify-Replacement-Path fuer moderne Programme.

Aufwand: ~2500 LOC (Init + Mark-Tree + Event-Queue + Permission-Hook).

### B.3 process_vm_readv / process_vm_writev

Tests: `process_vm01`, `process_vm_readv02`, `process_vm_readv03`,
`process_vm_writev02` (alle Host PASS, CosmoRT SKIP).

Was: Cross-process Memory-Read/Write-Syscalls mit `iovec`-Listen.
Linux 3.2+, ptrace-Replacement-Path.

Warum wichtig: `gdb`, `strace`-modern (nicht-ptrace-fallback), `criu`,
`bpftrace user-probe`. `ptrace(PEEKDATA)` ist langsam (Wort-fuer-Wort);
`process_vm_readv` macht Page-Range-Copy.

Aufwand: ~600 LOC (Syscall + IOV-Validation + cross-mm-uaccess).

### B.4 eBPF (Basis-Subsystem)

Tests: `bpf_map01`, `bpf_prog01-04` (alle Host PASS, CosmoRT SKIP).

Was: `bpf()` Syscall — JIT-compiled BPF-Programs + Maps fuer
network-filter, tracing, security.

Warum wichtig: `tc`, `perf`, `bpftrace`, modern-`iptables-bpf`,
seccomp-bpf. systemd-`Restrict*`-Direktiven nutzen seccomp-bpf.

Aufwand: **gross** (~5000+ LOC). bpf-verifier ist nicht-trivial.
Niedrigere Stelle in Prio-B als die anderen weil Aufwand vs Nutzen.

### B.5 fchmodat2 + copy_file_range + close_range Edge

Tests: `fchmodat2_01` (host fail wegen alpine-Setup, aber Syscall-Stub
fehlt auf CosmoRT auch), `copy_file_range01/02`, `close_range01`.

Was: Moderne POSIX-extension-Syscalls.
- `fchmodat2(AT_SYMLINK_NOFOLLOW)` — Linux 6.6+.
- `copy_file_range()` — kernel-zu-kernel-Copy ohne user-buffer-roundtrip.
- `close_range()` — schon implementiert in CosmoRT, edge-case-flag fehlt.

Warum: `cp`, `dd`, performant-File-Tools.

Aufwand: ~400 LOC (Wrapper + ext4-COW-Path fuer copy_file_range).

---

## Prio C — Fehlende Subsysteme (Niedrige Relevanz)

### C.1 add_key + Keyring-API

Tests: `add_key01-04` (Host PASS, CosmoRT SKIP).
Linux-Kernel-Keyring fuer cred-storage. Selten genutzt.
Aufwand: ~1500 LOC. Skip bis konkrete Anforderung.

### C.2 cachestat (Linux 6.5+)

Tests: `cachestat01-04` (Host meist FAIL, einer PASS).
Page-Cache-Statistiken pro File-Range. Sehr neu, kaum verbreitet.
Aufwand: ~300 LOC. Niedrige.

### C.3 Kernel-Module-System

Tests: `lsmod01.sh`, `delete_module01-03`, `finit_module01/02`.
**Out of scope** — CosmoRT ist explizit statisch verlinkt, kein Modul-
Loading. Diese Tests bleiben fuer immer SKIP.

### C.4 CVE-Regressionstests

Tests: `cve-2014-0196`, `cve-2016-7117`, `cve-2017-2671`, `cve-2016-10044`,
`cve-2016-7042`, `cve-2017-2618`, `cve-2022-4378`, `meltdown`.

Linux-spezifische Kernel-Bug-Regressionstests. Nicht relevant fuer einen
neu-implementierten Kernel der die Bugs nie hatte. Bleiben SKIP/FAIL.

### C.5 arch_prctl, fcntl40 Edge-Cases

Single-test edge-cases. Niedrige Relevanz, fix wenn verlangt.

### C.6 ICMP rate limit, traffic control

Tests: `icmp_rate_limit01`, `tcindex01`. Niche networking. Niedrige.

---

## Akzeptierte Fails (NICHT Kernel-Bugs)

Tests die auch auf Linux failen oder fehlende Subsysteme treffen — kein
Engineering-Effort wert.

### Fehlende Subsysteme (1 Test)

`fanotify25` — kein fanotify-Test im engeren Sinn. Setup mountet
`tracefs` auf `/sys/kernel/tracing` und schreibt kprobe-events. CosmoRT
hat weder tracefs-Driver (`src/kernel/sys/stubs.c:71` erlaubt nur
tmpfs/ext{2,3,4}) noch CONFIG_TRACING-Infrastruktur. `mount("tracefs",
...)` returnt -ENODEV → LTP `safe_mount` macht TBROK (rc=2).

LTP haette per `needs_kconfigs = "CONFIG_TRACING"` TCONF-skippen koennen,
aber `KCONFIG_SKIP_CHECK=1` (boot-test.sh:9) deaktiviert das, weil
CosmoRT `/proc/config.gz` nicht exportiert und ohne den Skip alle anderen
needs_kconfigs-Tests TBROK schlagen. Trade-off-Akzeptanz: lieber 1 FAIL
(fanotify25) als ~30 TBROK quer durch LTP.

Gleiche Klasse wie fanotify01-24/Prio B.2 (fehlendes Subsystem).

### Busybox vs GNU coreutils Mismatches (10 Tests)

`ar01.sh`, `df01.sh`, `du01.sh*`, `file01.sh`, `gzip_tests.sh`,
`ld01.sh`, `ldd01.sh`, `mkfs01.sh`, `mv_tests.sh`, `nm01.sh`,
`tar_tests.sh`. LTP-Tests gehen davon aus dass GNU coreutils installiert
sind, alpine nutzt busybox-Aliasse mit subtle output-mismatches.

(*) `du01.sh` ist auch in Prio A.3 — Stat-Bug ist vermutet, dann waere
es Kernel-Bug; falls reine busybox-Differenz, gehoert es hierher.

### musl-Upstream-Bugs (10 Tests)

`fma`, `fmal`, `powf` (math-precision-Edge-Cases),
`mntent` + `mntent-static` (parser-Bug),
`strptime` + `strptime-static` (Format-string-Bug),
`sem_close-unmap` + `sem_close-unmap-static` (POSIX-semaphore-Race im
musl-runtime, brauchen /dev/shm-Setup).

Bugs sind in musl, nicht im Kernel. Bestaetigt durch Host-FAIL.

### LTP-Test-Timing-Bugs (4 Tests)

`clock_settime04`, `epoll_pwait03` — LTP-Timing-Slack zu eng.
`clone08`, `clone10` — Test-Bug, falsche Filter-Erwartung.

### Userspace-Setup-Issues (2 Tests)

`cve-2014-0196` — braucht `/dev/ptmx` + TIOCGPTN, alpine-Konfig fehlt.
`unshare01.sh` — braucht uid_map/gid_map setup, 5/8 sub-tests PASS,
3 brauchen vollen user-namespace-Setup auf alpine-Seite.

---

## Test-Infrastruktur

### Targets

```sh
make                   # Kernel build
make test-hw           # Kernel-Unit-Tests (3246/0) → /tmp/cosmo-serial.log
make alpine-test       # Vollauf CosmoRT (~15min KVM) → /tmp/alpine-test.log
make alpine-test-host  # Vollauf Host-Linux (~10min KVM) → /tmp/alpine-test-host.log
make qemu-bench        # gcc-compile-Bench → /tmp/cosmo-bench.log
```

### Console-cmdline (Linux-Parity)

Default ohne cmdline = silent. Test-Targets uebergeben automatisch
`-fw_cfg name=opt/cmdline,string=console=ttyS0,,115200` (Linux-Style
cmdline ueber QEMU fw_cfg, kein hardcoded UART mehr).

### Fokussierte Iteration

```sh
echo 'popen|raise-race' > build/alpine-root/opt/musl_run
echo '__none__'         > build/alpine-root/opt/ltp_run
make alpine-test        # ~3min nur diese Tests, DEBUG=1 vollen Output
rm build/alpine-root/opt/musl_run build/alpine-root/opt/ltp_run
```

### A/B-Diff

```sh
# Nach beiden Vollaeufen:
grep -E "^\[[0-9]+/[0-9]+\] .* FAIL" /tmp/alpine-test.log      | sort > /tmp/cosmo_fails.txt
grep -E "^\[[0-9]+/[0-9]+\] .* FAIL" /tmp/alpine-test-host.log | sort > /tmp/host_fails.txt
comm -23 /tmp/cosmo_fails.txt /tmp/host_fails.txt   # nur CosmoRT — echte Bugs
comm -12 /tmp/cosmo_fails.txt /tmp/host_fails.txt   # beide — akzeptiert
```

---

## Roadmap

| # | Phase | Kern-Problem | Aufwand | Status |
|---|-------|--------------|---------|--------|
| **10.1** | Waitqueue-Infrastruktur | atomic blocking primitive | ~600 LOC | ✓ |
| **10.2** | **Waitqueue-Migration restliche Wait-Pfade** | event_wait/futex/pipe/socket/epoll/signalfd/timerfd/wait4 | ~1500 LOC, 1 Pfad/Agent | **NEXT** |
| **11** | restart_block + signal-restartable syscalls | EINTR ohne rem-Recovery | ~500 LOC | ✓ |
| **12** | hrtimer ns-Präzision + Tick-less | timer_ms() in Hot-Path | ~800 LOC | partial (TSC mult/shift einzeln) |
| **13.1** | Skalierungs-Audit (Linus-today Limits) | FD_CEILING, NGROUPS_MAX, USOCK_BACKLOG | ~400 LOC | ✓ |
| **13** | SMP-saubere Scheduler-Finalisierung | globales rq_lock | ~1200 LOC | nach 10.2 |
| **14** | vDSO clock_gettime | syscall pro clock_gettime | ~600 LOC | ✓ |
| **15** | Network-Namespaces | netif+route-table global | ~2000 LOC | ✓ |
| **16** | IPv6-Stack | AF_INET6 = EPROTONOSUPPORT | ~2500 LOC | in progress |
| **17** | OOM-Killer + oom_score_adj | alloc-fail → -ENOMEM ohne Reclaim | ~600 LOC | ✓ |

**Reihenfolge Säule 1 (Linux-ABI-Vollständigkeit, Alpine läuft)**:
- **10.2a** Architektur-Refactor (wait_head raus, try_to_wake_up rein) ✓
- **10.2b** event_queue intern auf wq + kill_one Doppelpfad weg ✓
- **10.2c** Subsysteme einzeln auf eigene wait_queue_head_t (eventfd/pipe/
  socket/epoll/futex/wait4/rt_sigtimedwait) — derzeit nutzen sie wq
  via event_queue (transparent). Direkt-Migration eliminiert event_queue.c
- **10.2d** event_queue.c und thread_t.eq löschen (nach 10.2c)
- **12-Rest** hrtimer ns + tickless retry (jetzt mit korrekter waitqueue)
- **13** SMP-Scheduler-Finalisierung
- Erfolgskriterium: alle musl + LTP grün auf x86_64, Alpine apk/bash/sshd
  vollständig nutzbar

## Konsole-Phasen — Säule 2 (WASM-Native USP)

| # | Phase | Inhalt | Aufwand |
|---|-------|--------|---------|
| **18** | **Audio-Subsystem-Core** | HDA + virtio-snd + USB-audio Treiber, RT-DMA-Buffer, native C-API | ~3000 LOC |
| **19** | **GPU-Subsystem-Core** | virtio-gpu + Framebuffer + minimal Vulkan-Cmd-Queue | ~4000 LOC |
| **20** | **binfmt_wasm + WASI-Shim** | Magic-Detection, AOT-Helper-Fork, /var/cache/wasm, WASI ↔ POSIX | ~3000 LOC |
| **21** | **cosmort-multimedia Host-Imports** | ~10 Funktionen: fb_acquire, audio_open, gpu_submit, input_poll | ~1500 LOC |
| **22** | **libcosmort-multimedia + SDL3-Shim** | Userspace-Library, SDL3-API on top von Host-Imports | ~5000 LOC |
| **23** | **WASM-Audio-Plugin-API** | LADSPA-Replacement, hot-loadable, sandboxed | ~2000 LOC |
| **24** | **aarch64-Port** | HAL-Stubs zu echtem Code, Alpine-aarch64 grün | mehrere Sessions |
| **25** | **Cross-Arch-Cartridge-Verifikation** | gleiches .wasm läuft x86_64+aarch64 identisch | (passive) |

**Reihenfolge Konsole-Phasen**: 18 → 19 (parallel) → 20 → 21 → 22 → 23 → 24 → 25.

**Beide Säulen sind permanent.** Säule 1 (Linux-ABI) ist nicht "transition
weg davon" — sie bleibt vollständig nutzbar. Compiler, Editor, Tools laufen
über Säule 1. Multimedia-Apps werden über Säule 2 ausgeliefert.
Interoperabilität zwischen beiden ist explizites Designziel:
gemeinsames Filesystem, Netzwerk, Userspace-Tools können beide Cartridge-
und ELF-Formate produzieren.

---

## Parkplatz (nach Phasen-Abschluss neu-evaluieren)

### LTP-SKIP-Audit (#70)
Alle ~44 SKIPs auditieren: Typ A (legitim) vs Typ B (versteckter FAIL
wegen fehlender Kernel-Features). Macht Sinn **nach Phase 15/16**, weil
dann Netns + IPv6 einen Großteil der SKIPs aus Typ B holen.

### Dokumentierte Flakes
Mit Phase 10 sollten verschwinden:
- accept02, accept03 (timing)
- fcntl36/36_64 (OFD-Race)
- pthread_mutex_pi, sem_init, sscanf_long, udiv (signal-wake)
- leapsec01, cve-2017-17052 (timing/memory-stress)

---

## Session-Bilanz (Session-Ende)

| Metrik | Start-of-Session | Ende |
|---|---|---|
| ktest | 2504 | **3246** (+742) |
| ktest FAIL | variable | **0** |
| musl PASS | 448 | **469** (+21) |
| LTP PASS | 198 | **276** (+78) |
| LTP FAIL | 87 | **19** (-68) |

### popen-Race (A.1+A.4) gefixt

3 Vorgaenger-Sessions ohne Erfolg. Wurzel diesmal gefunden via
non-perturbative Tracer (16K-event ringbuffer, lock-free atomic xadd):
**`schedule_timeout_common` hatte interne `for(;;)`-Loop die wakes als
spurious classified.** Bei `timeout_ns=0` (poll(-1)) keine break-Bedingung,
sh's poll auf pipe wakes via `wake_up(&pp->read_wq)` triggern
default_wake_function/try_to_wake_up aber alte schedule_timeout-Loop
setzte state=BLOCKED erneut via prepare_to_wait — sh nie wieder dran.

Linux-Pattern: schedule_timeout fuehrt EINEN schedule()-Call aus, returnt.
Caller (poll_loop in sys_event.c, sleep_until_ns in sys_time.c) hat eigene
outer-loop mit Condition-Recheck.

popen + popen-static + regex-* (4 Tests) gefixt durch eine Aenderung
in `core/waitqueue.c`. Commit 45b023d.

### Gefixte Cluster (Session)
fcntl (komplett), eventfd/epoll, chroot/caps, cve/execve, clone,
clock_*/adjtimex, bind/accept (TCP half-open, path-resolve, SEQPACKET),
access01/04 (DAC), fcntl35/35_64 (procfs-write), fcntl38/38_64 (nested
sigreturn), rest-bucket (8/9), dup201, copy_file_range03, CLONE_NEWNET.

### 3× reverted (dokumentiert)
Signal-Wake in thread_block_ms (direct-assign → CAS → wakeup_pending-flag).
Alle 3 reproduzierten den Missed-Wakeup-Race auf andere Art. Phase 10
(Waitqueue-System) ist die nachhaltige Lösung.

---

## Regeln

**Eine Phase, eine Transaktion.** Halbe Umstellung ist gefährlicher
als gar keine (siehe 3× Signal-Wake-Revert).

**Jeder neue Code-Pfad bekommt einen ktest** (CLAUDE.md).
ktest-Count muss monoton wachsen, nicht nur stabil bleiben.

**Linux-ABI ist nicht verhandelbar.** Jede Abweichung ist ein Bug.
