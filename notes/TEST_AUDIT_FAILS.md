# Test-Audit — Aufgedeckte Bugs

Session fuer Pool-Migrations-Audit. Baseline `2429/0`, nach Test-Audit
`2502/3`. +73 passes, +3 neue fails in neuen Tests. Nach Fixes `2505/0`.

## Übersicht

| # | Test                                   | Klasse | Fail-Typ                   | Status |
|---|----------------------------------------|--------|----------------------------|--------|
| 1 | `event/flood_epoll`                    | A      | `epoll_ctl ADD` ab 65. fd scheitert | FIXED (Watch-Slab, commit 09f07ce) |
| 2 | `event/flood_epoll`                    | A      | `epoll_wait` liefert nur 64 ready   | FIXED (Folge #1) |
| 3 | `stress/shared_mmap`                   | C      | Parent sieht 0 statt 0xCAFEF00D     | FIXED (Page-Cache owns refs) |

## Fail-Details

### 1+2 epoll statisches Pool

```
[EVENT_FLOOD_EPOLL]
  PASS  epoll_create1
  PASS  opened >256 pipes
  FAIL  epoll_ctl ADD all (got=64 expected=300)
  FAIL  epoll reports >256 ready (got=64 min=257)
```

**Symptom:** `epoll_ctl(EPOLL_CTL_ADD)` gibt ab 65. Eintrag Fehler (oder wird
ignoriert). `epoll_wait` meldet nur 64 ready-fds.

**Hypothese:** `src/kernel/event/epoll.c` hat ein verbleibendes statisches
Array `struct epoll_item items[64]` pro epoll-Instance. Kein slab-dynamisches
Growth. **Verletzt CLAUDE.md Ressourcen-Design:** jeder Prozess kann mit einer
einzigen epoll-Instance nur 64 fds beobachten, unabhängig von `RLIMIT_NOFILE`.

**Impact:** musl `poll()` via epoll emulation bricht bei >64 fds. Alpine
OpenRC + sshd + getty könnte >64 fds aktiv pollen.

**Fix-Aufwand:** Medium. epoll-Items in Slab-Cache, Hash-Table oder
rb-tree für Lookup.

### 3 SHARED_MMAP cross-process

```
[SHARED_MMAP_CROSS_PROC]
  PASS  open file
  PASS  mmap shared parent
  PASS  fork
  PASS  child ok
  FAIL  parent mmap sees child mmap write (got=0 expected=3405705229)
```

**Symptom:** Parent `mmap(MAP_SHARED, fd, 0)` sieht nicht, was Child via
eigener `mmap(MAP_SHARED, fd, 0)` geschrieben hat. Manuelles `sync()` im
Child macht es sichtbar (verifiziert).

**Hypothese:** `mm_mmap` für `MAP_SHARED` file-backed alloziert pro Caller
eine eigene Page statt über die bcache/page_cache-Inode zu gehen. Linux'
`struct address_space` hält genau eine page pro (inode, offset).

**Impact:**
- Dynamischer Linker `ld-musl-x86_64.so.1` mappt libc.so mehrfach (einmal
  für readonly header parse, dann für .text).
- `shm_open` / `/dev/shm` basiert darauf.
- PostgreSQL / multi-process cache braucht es.

**Fix-Aufwand:** Groß. Page-Cache-Integration, inode-to-page-Mapping,
Dirty-Tracking für msync.

## Klassen-Uebersicht

| Klasse | Tests | Pass | Fail | Bemerkung                                          |
|--------|-------|------|------|----------------------------------------------------|
| A — Pool-Growth | 9  | 7 | 2 | epoll-Limit aufgedeckt                              |
| B — RLIMIT      | 5  | 5 | 0 | NPROC/FSIZE/CPU/NOFILE korrekt verdrahtet           |
| C — Real-World  | 7  | 6 | 1 | SHARED_MMAP inter-process Page-Cache fehlt         |
| D — Loader-Sim  | 3  | 3 | 0 | mprotect-Loop, many-reads, fixed-mmap ok           |
| E — Races       | 3  | 3 | 0 | close-race/parallel-fork/PID-reuse ok (kurzes Fenster) |

## Nicht getestet / uebersprungen

| # | Feature         | Grund                                                          |
|---|-----------------|----------------------------------------------------------------|
| 4 | ARP Growth      | Braucht Test-Stub für arp_cache_add; kernel-intern             |
| 5 | TCP OOO > 4     | Braucht TCP-Testinjection; nur im echten Netzwerk triggerbar   |

## Priorität (für Kernel-Fix-Agent)

1. **epoll-Pool dynamisch** — blockiert musl-Tests, Alpine-Boot direkt.
2. **SHARED_MMAP cross-process** — blockiert dynamic linker, gängige libc-Tests.
3. (Nicht getestet) ARP-Cache-Growth — unter `net/arp.c` prüfen ob fixes Array.

## Commits

- `5fa0365` test: Klasse A Pool-Growth-Pfade
- `f388360` test: Klasse B RLIMIT-Enforcement
- `789e55b` test: Klasse C Real-World-Stress
- `858983b` test: Klasse D Dynamic-Linker-Pattern
- `905d3e6` test: Klasse E Korruptions-Detection / Races
