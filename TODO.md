# CosmoRT — TODO

**Test-Status:** test-hw 3246/0 · musl 469/9 · LTP 277/18/155

Linux/POSIX/musl-ABI-kompatibler Kernel. Modern, schlank, performant. Kein Legacy.

Aufgabenformat: jede Subtask ist allein commit-bar (~30–60 min). Workflow für jeden Bug: `test-hw`-Repro vor Fix, dann beide Suiten grün, keine Regression (CLAUDE.md §9).

---

## Erledigt

- **Bug 1 — `net/tcp_hash_multi` TIMEOUT** (Commits `c455b67`, `2710886`)
  - Wurzel war nicht Port-Kollision (verworfen), sondern fehlende TCP-Retransmission. `net_tcp_retransmit` war registriert, aber nirgends armed → fire-and-forget. Bei e1000-RX-Drop kein Retry, Recv hing.
  - 1.3a: `socket_read`/`do_recvfrom` mit `schedule_timeout`-Backstop (kein 10s-Watchdog mehr bei Drop ohne Wake).
  - 1.3: RFC-6298-simplified Retransmission — `retx_buf[1460]` per Connection, arm bei `net_tcp_send`/connect, cancel bei ACK-drain, RTO-Backoff ×2 bis 8s, nach 5 Retries RST. SYN-Retx und Daten-Retx via `snd_nxt`-Rewind-Trick (wie keepalive).
  - test-hw 3243/1 → 3246/0; alpine-test unverändert (kein TCP-Test in den FAILs).

---

## Aktive Bugs

### Bug 2 — LTP `clone08` TID/PID-Identität

`ctid != getpid()`, `ptid != getpid()`. Linux: Main-Thread hat `tid == tgid == pid`. CosmoRT: getrennte ID-Räume (`next_pid`/`next_tid`).

Risiko: Wenn `tid_table` extern andere Semantik hat (z.B. `/proc/<tid>`), kann 2.1 Wave-of-Fixes auslösen. Vor Commit Grep auf `tid_table`/`pid_table`.

- [ ] **2.0 test-hw Repro** — Unit-Test: `clone(CLONE_PARENT_SETTID|CHILD_SETTID)` und prüfen `ctid==ptid==getpid()` für Main-Thread. *~30 LOC.*
- [ ] **2.1 Unified ID-Allocator** — `next_pid`/`next_tid` zu einem `next_id` vereinen; `proc_alloc`+`thread_alloc` ziehen aus selbem Counter. Reihenfolge in `process_fork.c:292/476`: `thread_alloc` ohne ID → `proc_alloc` zieht ID → `ct->tid = child->pid`. *~80 LOC, 3 Files.*
- [ ] **2.2 Sub-Thread-IDs aus Unified-Raum** — `process_fork.c:546-557` `is_thread`-Pfad: `ct->tid = alloc_next_id()`. *~20 LOC. Test: gettid01 + pthreads-Smoke.*
- [ ] **2.3 TGID-Cleanup** — Grep-Pass: `thread->tid`-Vergleiche zur Main-Thread-Erkennung durch `thread == proc->main_thread` ersetzen. *~10–30 LOC.*

---

### Bug 3 — musl `sem_close-unmap` Segfault

Use-after-unmap. CR2 zeigt auf gemappte Sem-Page. Auf Host PASS → CosmoRT-spezifisch. Verdacht: `tmpfs_file_ops` hat kein `.mmap` → `/dev/shm`-Mappings laufen anon → zwei Prozesse bekommen verschiedene phys Pages.

- [ ] **3.0 Invest** — musl-Source `sem_close-unmap.c` lesen. Test: zwei Forks mappen dieselbe `/dev/shm/sem.X`, `phys_addr` der Page vergleichen. Bestätigt Page-Cache-Lücke? *(0 LOC, kein Commit)*
- [ ] **3.1 test-hw Repro** — Unit-Test: zwei Tasks `mmap(MAP_SHARED)` derselben tmpfs-Datei, schreiben+lesen, identische Page erwarten. *~50 LOC.*
- [ ] **3.2 tmpfs page-cache-fähiger mmap** — Wenn Page-Cache existiert: `tmpfs_file_ops.mmap = tmpfs_mmap` mit `(file_ino, offset)`-Lookup, ~80 LOC. **Wenn nicht**: separate Phase, kein 60-min-Subtask — Page-Cache bauen ist Fundament.
- [ ] **3.3 do_mmap MAP_SHARED-Pfad** — `sys_mem.c:do_mmap`: bei `MAP_SHARED` + tmpfs-Backend physische Page aus Page-Cache statt frische `page_alloc`. *~30 LOC.*

---

### Bug 4 — LTP `epoll_pwait03` Timer-Slack (462µs früh)

Wurzel: `epoll.c:436+459` deadline = `timer_ms() + timeout` (ms-Granularität), dann `*1e6` zu ns. Sub-ms truncated. 2-ms-timeout kann real 1.x ms werden.

- [ ] **4.0 test-hw Repro** — Unit-Test: 2000µs-Timeout → schlafen, `clock_gettime(CLOCK_MONOTONIC)` vor/nach, Diff muss ≥ 1900µs sein. *~30 LOC.*
- [ ] **4.1 Interne ns-Deadline** — `epoll.c:do_epoll_wait`: `deadline_ns = hrtimer_now_ns() + (uint64_t)timeout * NSEC_PER_MSEC`, kein Umweg über `timer_ms()`. ms-`deadline`-Variable löschen. *~10 LOC.*
- [ ] **4.2 epoll_pwait2 ns-Pfad** — `do_epoll_pwait2`: deadline direkt aus `timespec` ohne ms-Umweg. `do_epoll_wait_ns`-Variante anlegen. *~15 LOC.*

---

### Bug 5 — LTP `unshare01` uid_map/gid_map + Mount-Propagation

3 Sub-Tests fail: `--user uid/gid` (kein uid_map), `--propagation shared --bind` (MS_SHARED ist no-op).

- [ ] **5.0 test-hw Repro** — Unit-Test: `/proc/self/uid_map` lesen+schreiben, sowie `mount(MS_BIND)` zwischen zwei Pfaden + `ls`. *~50 LOC.*
- [ ] **5.1 procfs uid_map/gid_map** — `/proc/self/uid_map` + `gid_map` als rw-Files. Read: `0 0 4294967295\n` (Identity). Write: validiert Format, no-op. *~60 LOC.*
- [ ] **5.2 MS_BIND echte Implementierung** — `vfs_bind_mount(source, target)`: target-dentry verweist auf source-inode. Falls `vfs_mount.c` keine Multi-Mount-Liste hat: 5.2a (Datenstruktur) + 5.2b (Bind-Logik) splitten. *~80–150 LOC.*
- [ ] **5.3 MS_SHARED/PRIVATE/SLAVE** — Mount-Eintrag um `propagation_flags`-Feld erweitern, gesetzt aber nicht propagiert (single-user). *~30 LOC.*

---

## Reihenfolge (Empfehlung)

| # | Subtask | LOC | Begründung |
|---|---|---|---|
| 1 | 4.0 + 4.1 | 40 | Klein, RT-Versprechen, niedrig hängend |
| 2 | 5.0 + 5.1 | 110 | uid_map isoliert, gut testbar |
| 3 | 2.0 + 2.1 | 110 | TID/PID — größtes Risiko, früh angehen |
| 4 | 3.0 + 3.1 | 50 | Klären ob Page-Cache existiert |
| 5 | 3.2/3.3 oder Phase | ? | Je nach 3.0-Befund |
| 6 | 2.2, 2.3 | 50 | Cleanup |
| 7 | 5.2, 5.3 | 110–180 | Mount-Propagation |

---

## Userspace-FAILs (kein Kernel-Fix)

Dokumentiert als TFAIL akzeptiert — busybox-vs-GNU oder musl-Bug:

- musl: `mntent`/`mntent-static`, `strptime`/`strptime-static`, `fma`/`fmal`/`powf`
- LTP: `file01`, `ldd01`, `nm01`, `tar_tests`, `ar01 -u`

---

## Strategische Lücken (out-of-scope, Bestandsaufnahme)

- **SMP echt:** `smp_num_cores()==1`, globales `rq_lock`, keine per-CPU-Runqueues. Voraussetzung für SCHED_FIFO mit CPU-Isolation.
- **aarch64:** 153 LOC Panic-Stubs, `entry.S` ist ein Kommentar. CLAUDE.md fordert Tier-1.
- **Stub-Audit:** `src/kernel/sys/stubs.c` 11 No-op-Returns + 2× `-ENOSYS` (CLAUDE.md §8 verbietet Stubs); `umount2` entfernt mount-entry nicht; `do_sched_setscheduler` lehnt SCHED_DEADLINE/BATCH/IDLE ab; `vt/input.c:14 INPUT_MAX_DRIVERS 4` static array (verstößt gegen §6).
- **TCP-Folgearbeit** (aufgedeckt durch Bug 1): per-skb send queue statt single-segment retx_buf; RFC 6298 SRTT/RTTVAR-Estimator; FIN-Retransmit; SACK-basiertes selektives RTX (RFC 6675); Timer-Callback-vs-sock_free Race (gilt auch für keepalive).
- **e1000 RX-Ring:** `NUM_RX_DESC=8` (drivers/pci/e1000.c:86) — bei N≥8 simultanen Verbindungen overflowt unter Last. Ohne Retransmission war das ein Hänger; mit Retransmit nur noch Latenz. Vergrößern auf 64 wäre billig.
