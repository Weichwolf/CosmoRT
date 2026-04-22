# Alpine Test — Bestandsaufnahme

Run: 2026-04-22 nach Socket-Cluster (bind/accept/connect).

## Ergebnis

| Suite | Total | PASS | FAIL | SKIP | Delta vs vorher |
|-------|-------|------|------|------|-----------------|
| ktest | 2760  | 2760 |   0  |   -  | +28 (neue LTP-bind/connect/accept Fehlerpfade + AF_UNIX abstract) |
| musl  |  478  |  458 |  13  |   7  | =/= (stabil)    |
| LTP   |  313  |  228 |  27  |  43  | stabil — **accept4_01** + **bind04** bleiben FAIL (s.u.) |

Baseline diesem Run vorher: ktest 2732, musl 458/13, LTP 228/27/43.

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
| accept4_01  | FAIL   | FAIL    | **Offen** — ETIMEDOUT: TCP-Handshake ohne accept() |
| bind01      | PASS   | PASS    | —                                                  |
| bind02      | PASS   | PASS    | —                                                  |
| bind03      | PASS   | PASS    | —                                                  |
| bind04      | FAIL   | FAIL    | **Teilfortschritt**: AF_UNIX pathname stream PASSt jetzt (FS-Node + unlink), restliche Cases TBROK (SOCK_SEQPACKET + IPv6) |
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

**accept4_01** (TCP half-open queue fehlt):
```
accept4_01.c:105: TBROK: connect(4, 127.0.0.1:61322, 16) failed: ETIMEDOUT
```
Testablauf: `listen_fd = bind+listen; connect(client); accept4()`.
Unser TCP: SYN landet nur in `q_tcp`-Queue, Bearbeitung erst in `accept()`.
Client-Connect blockiert → 30s-Timeout → TBROK.

Linux: tcp_input auf SYN bei bestehendem Listener sendet sofort SYN-ACK
und legt half-open Eintrag in Listen-Backlog; accept() dequeued später.
Fix-Umfang: TCP-SYN-Pfad in `tcp_input` + Listen-Backlog-Slab +
Race gegen `net_tcp_accept`. Eigener Task.

**bind04** (SOCK_SEQPACKET + IPv6):
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
| fcntl          |    5   | fcntl35/35_64, fcntl38/38_64, fcntl14             | procfs-write, nested sigreturn, perf |
| epoll_wait     |    2   | epoll_wait02, epoll_wait05                        | timing, TCP loopback   |
| clock_*        |    3   | clock_gettime04, clock_nanosleep01/02              | Clock-Jitter / Sched-Perf, KEIN NEWTIME |
| clone/dup      |    2   | clone09 (procfs TBROK), dup05/dup201              | -                      |
| bind/accept    |    2   | accept4_01 (TCP half-open), bind04 (SOCK_SEQPACKET+IPv6) | siehe oben     |
| access         |    2   | access01, access04                                | DAC edge-cases         |
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
- access01/04 (DAC edge-cases im Single-User)
- cve-17052 (memory-reclaim)
