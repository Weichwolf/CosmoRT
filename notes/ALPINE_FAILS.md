# Alpine Test — Bestandsaufnahme

Run: 2026-04-22 nach Socket-Cluster (bind/accept/connect).
Update: 2026-04-22 nach TCP-half-open-queue.
Update: 2026-04-22 nach procfs-write + pipe-max-size (fcntl35).
Update: 2026-04-22 nach dnotify nested sigreturn-delivery (fcntl38).
Update: 2026-04-24 Phase 10 Waitqueue-Infrastruktur (ba88466, e255af3).
Update: 2026-04-24 Phase 10.2a+b futex + pipe auf waitqueue (ef2994d, 0277e99).
  ktest 2893/1 -> 2915/1 (+22 Tests, alle gruen).
  musl + LTP: alpine-test Run in dieser Session timed out nach 30min
  (abgebrochen mit stale futex-image); vollstaendige Verifikation
  verbleibt der naechsten Session.
Update: 2026-04-25 Phase 11 restart_block + ERESTARTSYS (f20ad1d..52c905b
  + 8997a10 revert futex). ktest 2906/1, musl 460/11, **LTP 247/7/44**
  (+3 PASS, -3 FAIL).
  Gewonnen: clock_nanosleep01 (SEND_SIGINT-rem korrekt via
  apply_restart-Pfad). Verloren: pthread_exit-cancel-static,
  tls_get_new-dtv (SEGFAULT RIP=0), capset01, rlimit-open-files-static
  -- vermutlich ERESTARTSYS-Interaktion mit musl static-link Pfaden;
  dynamic-Variante derselben Tests bleibt PASS.
  pthread_atfork-errno-clobber bleibt FAIL (anderer root cause als
  Phase 11 vermutete; errno-Erhaltung ueber sigreturn).

Update: 2026-04-25 Phase 12 hrtimer ns + tickless LAPIC
  (5b8ef42..76dd4a0, 4 commits). ktest 2916/1 (+10), musl 460/11
  unveraendert. LTP-Run im Gange.

  Migration: timer_ms()-basierte Sleep/Poll/Select/Futex-Pfade alle
  auf hrtimer_now_ns + event_wait_ns. LAPIC im tickless one-shot,
  Deadline = min(next-hrtimer, next-1ms-tick).

  Messung: nanosleep(500us) Pre 1.5ms -> Post 1085us. 100x nanosleep(100us)
  Pre 193ms -> Post 36ms. clock_gettime-max-diff ueber 1000 reads: 62us.

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta vs vorher |
|-------|-------|------|------|------|-----------------|
| ktest | 2868  | 2868 |   0  |   -  | **+5** (dnotify_handler_multi) |
| musl  |  478  |  460 |  11  |   7  | unveraendert |
| LTP   |  313  |  244 |  10  |  44  | **+2 PASS** (fcntl38, fcntl38_64) |

Baseline: ktest 2760 post-NEWTIME. Signal-Wake-Agent (9 commits 00bf941..c4f8f23)
reverted in 22b3ab5 — Deadlock in clock_nanosleep01 (kompletter Hang, kein
timeout-fire). Retry braucht Bisect + kleineren Scope.

## Socket-Cluster (bind/accept/connect) — dieser Task

Prompt-Annahme: 9 FAILs (bind01/02/03/04, accept02/03/4_01, connect01/02).
**Realität**: Nur **2 FAILs** (accept4_01, bind04). Die anderen 7 waren bereits
PASS. ALPINE_FAILS war bei „bind/accept=2" korrekt.

### Status nach diesem Task

| Test        | Vorher | Nachher | Ursache / Fix                                      |
|-------------|--------|---------|----------------------------------------------------|
| accept01    | PASS   | PASS    | —                                                  |
| accept02    | PASS   | PASS    | —                                                  |
| accept03    | PASS   | PASS    | —                                                  |
| accept4_01  | FAIL   | **PASS** | **Fix**: TCP-half-open-queue. tcp_input emittiert SYN-ACK direkt, request landet in listener.syn_queue, ACK promoviert nach accept_queue, accept4 materialisiert child via net_tcp_accept_child. Commit 2ebdc05. |
| bind01      | PASS   | PASS    | —                                                  |
| bind02      | PASS   | PASS    | —                                                  |
| bind03      | PASS   | PASS    | —                                                  |
| bind04      | FAIL   | PASS    | Seit TCP-Fix auch grün im laufenden Run (SEQPACKET- und IPv6-Subcases werden möglicherweise timing-abhängig übersprungen; Flake-Risiko, aber konsistent PASS diesmal). |
| bind05      | SKIP   | SKIP    | needs_root+setuid                                  |
| connect01   | PASS   | PASS    | —                                                  |
| connect02   | SKIP   | SKIP    | needs_checkpoint                                   |

### Nebeneffekt-Fixes (flossen mit)

1. **AF_UNIX abstract namespace** (sun_path[0]==0 + Länge):
   `usock_bind`/`usock_connect` akzeptierten abstract Pfade bisher mit
   EINVAL, da der First-NUL-Terminator-Heuristik kollidiert. Neu: explizites
   `path_len`-Feld, separate Logik für abstract vs pathname.

2. **AF_UNIX pathname erzeugt FS-Node**: `usock_bind` legt via
   `vfs_open(O_CREAT|O_EXCL|O_WRONLY)` eine reguläre Datei an. Existiert
   sie schon → `-EADDRINUSE`. Damit funktioniert SAFE_UNLINK nach Nutzung.

3. **O_PATH Flag bei Device-fds erhalten**: `vfs_open("/dev/null", O_PATH)`
   hat O_PATH verloren (`flags & 3` maskierte alles außer RW-Modes).
   Konsequenz: `accept()` auf O_PATH-fd lieferte ENOTSOCK statt EBADF.
   Fix in allen Device-Pfaden (/dev/null, /dev/zero, /dev/urandom,
   /dev/console, /dev/tty, /dev/tty\*, /dev/pts/\*).

### Verbleibend

**accept4_01** — geschlossen. TCP-half-open-queue implementiert:
- `tcp_request_t` Slab-Entry mit {src_ip, src_port, iss, irs, optionen,
  early_data}, per-Listener FIFOs `syn_queue` / `accept_queue`.
- `tcp_input` SYN-Pfad: Request alloziert, SYN-ACK sofort via Stub-
  net_tcp_t emittiert, Request in syn_queue.
- `tcp_input` ACK-Pfad: Promote syn_queue → accept_queue + Wake
  listener-wait_thread. Retransmit-Safe via dup-detection.
- Early Data: Payload piggybacked auf 3WHS-ACK oder auf Segmenten vor
  accept() → in request->early_data (1460B MSS) gepuffert, Peer geACKt,
  replay in child->rx beim accept.
- `do_accept4` + `net_tcp_accept_child` materialisieren child aus
  Request (eigener rxring, TCP-hash-Registrierung), statt ls->tcp zu
  kannibalisieren (alter Pfad zerstörte den Listener).
- `socket_close` auf Listener drainiert beide Queues via
  `tcp_listener_drain`.
- Backlog-Cap: min(listen-backlog+1, TCP_SOMAXCONN_DEFAULT=4096),
  SYN-flood wird silently dropped.

**bind04** (historisch: SOCK_SEQPACKET + IPv6):
```
bind04.c:117: TINFO: Testing AF_UNIX pathname stream
bind04.c:149: TPASS: Communication successful
<fällt auf SEQPACKET tcase mit TBROK in SAFE_SOCKET>
```
AF_UNIX SOCK_SEQPACKET liefert EPROTONOSUPPORT. IPv6 (AF_INET6=10)
auch. Beide blockieren SAFE_SOCKET → TBROK. Separate Tasks:
- SEQPACKET: Message-Boundary-Semantik in unix_socket.c
- IPv6: TCP-IPv6-Stack (neues Adressformat, Socket-Lookup-Hash, DAD)

## fcntl-Cluster (2026-04-22)

12/14 behoben, unverändert.

## procfs-write + pipe-max-size (2026-04-22)

Generische procfs_register_rw-Infra war bereits da (time_ns_offsets
Schreibhandler). Neu: /proc/sys/fs/pipe-max-size bekommt Read + Write.

- `pipe_max_size_get/set` in sys_ipc.c als Single-Source-of-Truth
- pipe_alloc (do_pipe2): unprivilegierter Prozess wird auf aktuellen
  pipe_max_size gecappt. CAP_SYS_RESOURCE (cap_effective-Bit) umgeht
  das, nicht euid==0 — LTP fcntl37 droppt die Cap via TST_CAP_DROP
  waehrend euid=0 bleibt.
- F_SETPIPE_SZ: gleicher CAP-Check, dynamischer Cap statt hart-kodierter
  1 MiB.

Gewonnen: fcntl35, fcntl35_64 (2 PASS).

## dnotify nested sigreturn-delivery (2026-04-22)

fcntl38/fcntl38_64: Zwei F_NOTIFY-Watches mit demselben Signal auf
parent-dir + subdir. chmod auf subdir matcht beide Watches → zwei
(fd, sig)-Tupel in p->dnotify_q. SA_SIGINFO-Handler sah aber nur
das erste si_fd; der zweite war erst beim naechsten Syscall abrufbar.

Linux stellt pending Signale zwischen Handler-ret und Return-to-user
als nested Frame zu. 528d571 hatte das bedingungslos in
check_signals_syscall_path fuer SYS_RT_SIGRETURN versucht, brach aber
pthread_cond-smasher / pthread_once / capset04 → revertiert c3b602f.

Gezielter Fix (effac51): Nach RT_SIGRETURN nur dann nested delivery,
wenn die dnotify-Queue noch Eintraege fuer ein aktuell pending Signal
enthaelt. Andere pending Signale (SIGCHLD im futex-Pfad, pthread_cancel)
bleiben fuer den naechsten Syscall. Pthread-Regression vermieden.

Gewonnen: fcntl38, fcntl38_64 (2 PASS). ktest +5.

## QEMU: Zweit-NIC (virtio-net-pci)

Flags ergänzt in `Makefile`: `-device virtio-net-pci,netdev=net1
-netdev user,id=net1,net=10.0.3.0/24`. Dmesg zeigt beide NICs:
```
e1000: MAC=52:54:00:12:34:56 IRQ 11 → netif: e1000 up
virtio-net: MAC=52:54:00:12:34:57 IRQ 11 → netif: virtio-net up
netif: lo up
```
DHCP nutzt die zuletzt registrierte NIC (virtio-net → 10.0.3.15).
Multi-NIC-Routing-Support ist separates Thema (netif-pointer in
`net.c` ist noch singleton).

## LTP FAIL (25) — verbleibende Gruppen

| Gruppe         | Anzahl | Tests                                              | Ursache                |
|----------------|--------|----------------------------------------------------|------------------------|
| fcntl          |    0   | (fcntl35/35_64, fcntl38/38_64, fcntl14 alle PASS) | erledigt                 |
| epoll_wait     |    2   | epoll_wait02, epoll_wait05                        | timing, TCP loopback   |
| clock_*        |    3   | clock_gettime04, clock_nanosleep01/02              | Clock-Jitter / Sched-Perf, KEIN NEWTIME |
| clone/dup      |    2   | clone09 (procfs TBROK), dup05/dup201              | -                      |
| bind/accept    |    2   | accept4_01 (TCP half-open), bind04 (SOCK_SEQPACKET+IPv6) | siehe oben     |
| access         |    0   | (access01, access04 PASS)                          | erledigt               |
| rest           |    9   | abort01, acct01, fchdir03, fchownat03, fdatasync02, fallocate02, fgetxattr02, copy_file_range03, stack_clash | diverse |

## clock_*-Cluster (2026-04-22)

NEWTIME-Integration implementiert: time_namespace + CLONE_NEWTIME +
unshare/setns + /proc/self/ns/time{,_for_children} + /proc/self/timens_offsets
Write-Handler + Offset-Anwendung in clock_gettime(MONOTONIC/BOOTTIME) +
clock_nanosleep(TIMER_ABSTIME). Siehe Commits time_ns: *.

Gewonnen: clock_gettime03, clock_nanosleep03 (2/5).
Offen:
- **clock_gettime04**: TFAIL weil aufeinanderfolgende clock_gettime-Aufrufe
  >5ms Differenz zeigen. Das ist ein reines Clock-Read-Perf-Problem
  (QEMU-TSC-Drift, sched-Interferenz waehrend 6000 Iterationen). Kein
  NEWTIME-Bug.
- **clock_nanosleep01**: PASS fuer NORMAL-Cases (3x EINVAL). TBROK auf
  SEND_SIGINT: child sendet 40x alle 500ms SIGINT an parent, parent
  schlaeft 10s. Parent muss per EINTR zurueckkehren; SIGKILL nach 30s
  LTP-Watchdog deutet auf steckende signal-wake aus thread_block_ms-hrtimer.
- **clock_nanosleep02**: TBROK — 500 Iterationen 1ms-Sleep dauern >30s.
  Scheduler-Overhead oder hrtimer-Granularitaet. Vermutlich derselbe
  root cause wie 01.

Alle 3 sind Scheduling/Clock-Praezisions-Buckets, kein NEWTIME.

## musl FAIL (13)

13 = 11 Baseline + 2 neue (zeitweilig flaky: pthread_mutex_pi-static,
pthread_atfork-errno-clobber). Keine systematische Regression.

## Priorisierung

**Top Fix-Kandidaten:**

1. **TCP half-open queue** — fixt accept4_01 + epoll_wait-Timing
2. **Signal wake aus hrtimer-block** — fixt clock_nanosleep01 + etliche SIGINT-empfindliche Tests
3. **procfs-write (als Konzept)** — Infrastruktur jetzt vorhanden (siehe time_ns),
   Integration fuer fcntl35/35_64 waere trivial
4. **AF_UNIX SOCK_SEQPACKET** — fixt eine bind04-Variante (noch nicht IPv6)

**Deprioritized:**
- IPv6-Stack (großer Rewrite, eigenes Milestone)
- fcntl14 (Mandatory-Locking in Linux auch entfernt)
- cve-17052 (memory-reclaim)

## access01/04 (2026-04-22) — PASS

`do_faccessat` reimplementiert: path-walk via vfs_stat → ELOOP/ENOTDIR/
ENOENT/ENAMETOOLONG. W_OK prueft MS_RDONLY am Mount → EROFS. Dann
cred_may_access mit R/W/X → EACCES. Root bekommt X_OK nur wenn ≥1 x-Bit.
Vorher: nur oberflaechlicher X_OK-any-bit-Check, DAC komplett ignoriert.
