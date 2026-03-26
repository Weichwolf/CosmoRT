# CosmoRT — Offene Punkte

Stand: 2026-03-26. 887 ktest PASS, 0 FAIL.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.
Audit: 27/27 Security-Fixes erledigt.

Netzwerk-Stack v2 Architektur: siehe notes/NETWORK.md

---

## Net Phase 0 — Quick-Fixes (npm unblockieren)

- [x] NET_QUEUE_SIZE 16→128
- [x] NET_TCP_RXBUF 4096→65536
- [x] NET_TCP_TIMEOUT_MS 5000→30000
- [x] TCP recv: timeout → -EAGAIN statt 0 (EOF)
- [x] TCP recv: non-matching Pakete re-queuen statt droppen
- [x] flock(73) Stub
- [x] MAX_SOCKETS 16→64
- [ ] test: npm install -g funktioniert
- [ ] test: claude update funktioniert

## Net Phase A — tcp.c extrahieren

- [x] TCP-Code aus net.c → tcp.c
- [x] Per-Socket Ringbuffer (64KB) statt globale q_tcp
- [x] TCP State-Machine (10 States, RFC 793)
- [x] Hash-Lookup tcp_find(sport, dport, src_ip)
- [x] test/unit/net/test_tcp_state.c
- [x] test/unit/net/test_tcp_rxring.c

## Net Phase B — udp.c extrahieren

- [x] UDP-Code aus net.c → udp.c
- [x] Per-Socket Demux statt globale q_udp_sock
- [x] test/unit/net/test_net_udp_echo.c

## Net Phase C — dispatch.c + arp.c + ip.c

- [x] Packet-Dispatch aus net.c → dispatch.c
- [x] ARP-Cache Modul → arp.c
- [x] IP-Header-Helpers → ip.c
- [x] test/net/test_arp_cache.c
- [x] test/net/test_ip_checksum.c

## Net Phase D — Userspace-Protokolle aus Kernel entfernen

~320 Zeilen Anwendungsprotokolle und Debug-Code gehoeren nicht in den Kernel.

### D1: Kernel-Module extrahieren (bleiben im Kernel, eigene Dateien)

- [x] DNS-Resolver net.c → src/kernel/net/dns.c (Kernel braucht DNS fuer Boot)
- [x] DHCP-Client net.c → src/kernel/net/dhcp.c (Kernel braucht DHCP fuer Boot)

### D2: In Userspace verschieben (raus aus dem Kernel)

- [x] net_http_get() → entfernt (Userspace: curl/wget/Node.js)
- [x] net_ping() → entfernt (Userspace: /bin/ping ueber Raw-Socket)
- [x] mDNS (net_set_hostname, mdns_respond, mdns_handle) → entfernt (Userspace-Daemon)
- [x] procfs_nettest() → entfernt (Userspace-Integrationstests)

### D3: Debug-Code aufräumen

- [x] serial_puts Debug-Ausgaben in tcp.c entfernt (8 Stellen)
- [x] IP-Adress-Formatierung in tcp.c entfernt

## RT/Compute Schnittstelle — include/kernel/core/rt.h

Problem: Keine saubere Abstraktion zwischen RT-Core und Compute-Cores.
Kommunikation laeuft ueber globale Queues mit Spinlocks.
RT-Core darf nie blockieren, Compute-Cores duerfen nie IRQ-State anfassen.

### RT/Compute-A: Grundprimitives

- [x] arch.h: arch_store_release, arch_load_acquire, arch_wmb, arch_rmb
- [x] arch.h: arch_dma_sync_for_device, arch_dma_sync_for_cpu (x86: No-Op, ARM64: Cache-Ops)
- [x] rt_channel_t: SPSC Lock-free Ringbuffer (atomic head/tail via arch_store_release/load_acquire)
- [x] rt_channel_push(ch, msg, len) — Producer-Seite, non-blocking
- [x] rt_channel_pop(ch, buf, len) — Consumer-Seite, non-blocking
- [x] rt_core_id(int index) — welcher physische Core ist RT-Core N
- [x] rt_is_current_rt() — true auf RT-Core(s), false auf Compute
- [x] test: rt_channel push/pop Roundtrip
- [x] test: rt_channel wrap-around bei vollem Buffer
- [x] test: rt_is_current_rt() korrekt auf beiden Core-Typen

### RT/Compute-B: IPI + Wake

- [x] rt_wake(int core_id) — IPI an Ziel-Core senden
- [x] sched_wake(thread_t *t) — markiert Thread runnable, sendet IPI falls anderer Core
- [x] IPI-Handler auf Compute-Core: Scheduler-Reschedule ausloesen
- [x] test: rt_wake IPI kommt an
- [x] test: sched_wake weckt schlafenden Thread auf anderem Core

### RT/Compute-C: TX-Ring (Compute→RT fuer Netzwerk-TX)

- [x] tx_ring_t pro NIC: SPSC rt_channel_t, Compute=Producer, RT=Consumer
- [x] send() Syscall: ip_send_raw() → rt_is_current_rt() → tx_ring_push auf Compute
- [x] net_poll() auf RT-Core: tx_ring_drain() → nic->send()
- [x] test: TX-Ring via TCP connect+send (test_tx_ring.c)

### RT/Compute-D: Timer-Wheel (RT-Core owned)

- [x] timer_wheel_t auf RT-Core: 1ms Granularitaet, 256 Slots, 64 Entries
- [x] rt_timer_request via SPSC Request-Ring (Compute→RT)
- [x] RT-Core: Timer-Wheel tick mit Elapsed-Time Berechnung
- [x] Timer-Actions: TCP Retransmit stub (Keepalive, DHCP spaeter)
- [x] test: Timer feuert nach Deadline
- [x] test: Timer-Cancel vor Deadline

### RT/Compute-E: Prioritaeten auf RT-Core

- [x] rt_poll_run() mit 6 Prio-Levels + Restart bei hoeherer Prio
- [x] Bounded Work: max_work pro Handler (Net-RX/TX: 64)
- [x] Handler returniert work_done → Restart von Prio 0 (max 4 Restarts)
- [x] test: Registrierung, Prio-Reihenfolge, Bounded Work (11 Tests)

### RT/Compute-F: Skalierung (Abstraktion, nicht Implementierung)

- [ ] RT_CORE_COUNT=1 (spaeter: 2 fuer IRQ-Split)
- [ ] rt.h abstrahiert ueber Core-Count, hardcoded Core 0 nirgends
- [ ] Escape-Hatches dokumentiert: Multi-RT, NIC-Offload, Protocol-auf-Compute

## ARINC 653 Partitionierung — RT-Core formal deterministisch

Architektur-Ziel: RT-Core (Partition A) hat bounded WCET. Kein Code-Pfad auf
dem RT-Core kann von Compute-Core-Aktivitaet beeinflusst werden. Partition-Grenze
= SPSC Lock-free Channels.

### ARINC-A: Shared Locks eliminieren

- [ ] Audit: alle Spinlocks die RT-Core acquiert auflisten
- [ ] sock_lock: RT-Core nie acquiren — bind/port-check via Channel an Compute
- [ ] buddy_lock: RT-Core nie acquiren — page_alloc nur auf Compute
- [ ] epoll_sleepers.lock: Lock-free Wake-List (atomic Linked-List oder CAS)
- [ ] slab Locks: RT-Core Pools voralloziert, nie slab_alloc zur Laufzeit
- [ ] test: RT-Core Code-Pfad Analyse — grep nach spin_lock in rt_poll Callchain

### ARINC-B: Dynamic Alloc auf RT-Core eliminieren

- [ ] Dedup: page_decref/page_free Ergebnis via Channel an Compute posten
- [ ] Hash-Engine: Kernel-Buffer Alloc nur auf Compute (Syscall-Handler), nicht RT
- [ ] Timer-Wheel: Pool beim Boot komplett voralloziert (bereits ✓)
- [ ] rt_channel Buffers: statisch, beim Boot alloziert (bereits ✓)
- [ ] Audit: kein page_alloc/slab_alloc/pages_alloc in rt_poll Callchain

### ARINC-C: IPI-Isolation

- [ ] TLB-Shootdown IPI: RT-Core filtern (nutzt keine User-PTEs)
- [ ] Reschedule IPI: RT-Core ignorieren (kein User-Scheduler auf Core 0)
- [ ] Nur explizite rt_wake() IPIs an RT-Core erlaubt
- [ ] test: TLB-Flush auf Compute loest keinen RT-Core Handler aus

### ARINC-D: Bounded Data Structures auf RT-Core

- [ ] tcp_find: Hash-Chain max 4 Eintraege (Overflow → Drop + Log)
- [ ] udp_find: Hash-Chain max 4 Eintraege
- [ ] rt_channel_push/pop: bereits O(1) ✓
- [ ] rt_poll Handler: max_work Limits bereits ✓
- [ ] Formal: WCET(rt_poll) = sum(WCET(handler_i) × max_work_i) berechenbar

### ARINC-E: RT-Core Memory-Isolation

- [ ] Dedizierter RT-Memory-Pool beim Boot carven (z.B. 16MB)
- [ ] RT-Core alloziert nie aus Compute-Buddy
- [ ] Compute-Buddy beruehrt RT-Pool nie
- [ ] Page-Tables fuer RT-Core Kernel-Mappings unveraenderlich nach Boot

### RT/Compute-G: SIMD Hash-Engine (RT-Core Idle Background-Optimizer)

RT-Core ist >98% idle. Nutze die Idle-Zeit fuer Background-Hashing mit
SSE2/SHA-NI. Kein fxsave noetig — RT-Core hat keinen Userspace-FPU-State.

Eine Hash-Engine, drei Konsumenten: RAM-Dedup, FS-Dedup, Cloud-Sync.

#### G1: SIMD SHA-256 auf RT-Core

- [x] include/kernel/crypto/sha256.h + src/arch/x86_64/sha256.c (FIPS 180-4, scalar C)
- [x] Eigene CFLAGS (ohne -mno-sse) fuer sha256.c
- [x] rt_poll RT_PRIO_HASH (P6, niedrigste): hash_poll() verarbeitet Jobs im Idle
- [x] rt_hash_submit/rt_hash_result via SPSC Channels (Compute→RT→Compute)
- [x] SYS_COSMO_RT_QUERY Subcmds 9/10 fuer Userspace Hash-Request/Result
- [x] test: 4 NIST Testvektoren (empty, "abc", 1Mx"a", 4K zeros)
- [ ] SHA-NI Upgrade (sha256rnds2, sha256msg1/2) wenn CPUID verfuegbar

#### G2: RAM-Dedup (KSM auf RT-Core)

- [x] include/kernel/mm/dedup.h: MM↔Dedup Interface (scan_next, merge, callbacks)
- [x] include/kernel/mm/page_age.h: PAGE_HOT/WARM/COLD, 2-Bit Age Counter
- [x] src/kernel/mm/dedup.c: Age-Tracking + PTE-Walking + Hash-Tabelle (4096 Buckets)
- [x] mm_dedup_scan_next/merge, dedup_on_page_free/cow_break
- [x] RT_PRIO_AGE (P7) + RT_PRIO_DEDUP (P8), bounded work
- [x] page_free() → dedup_on_page_free, COW-Fault → dedup_on_cow_break
- [x] /proc/ksm Statistik
- [x] test: merge, skip_hot, cow_break, stats (17 Assertions)

#### G3: CosmoFS Block-Dedup (Write-Path Offload)

- [ ] write() → COW-Append (sofort, kein Hash) → Hash-Job an RT-Core
- [ ] RT-Core: SHA-256 → Dedup-Tabelle Lookup
- [ ] Duplikat: neuen Block freigeben, Pointer auf existierenden
- [ ] Unique: Hash in Dedup-Tabelle eintragen
- [ ] Write-Latenz: ~0.5µs statt ~5µs (Hash aus Hot-Path entfernt)
- [ ] test: zwei identische Dateien schreiben → nur 1 Block auf Disk

#### G4: Cloud-Sync Hash-Readiness

- [ ] RT-Core berechnet Hashes aller CosmoFS-Blocks im Idle vor
- [ ] Sync-Start: Hash-Diff sofort verfuegbar, kein Warten auf Berechnung
- [ ] Dirty-Tracking: nur geaenderte Blocks neu hashen
- [ ] Abhaengigkeit: CosmoFS v2 (COW + Content-Addressing)

## Net Phase E0 — Polling eliminieren (Sleep/Wake statt Busy-Wait)

- [x] wait_thread Feld in net_tcp_t und udp_sock_t
- [x] sock_block_thread(): generische Blocking-Hilfsfunktion (save state, timer, epoll wake)
- [x] tcp_input() auf RT-Core: sched_wake(wait_thread) nach Daten/FIN/RST/SYN-ACK
- [x] udp_input(): sched_wake(wait_thread) nach Paket-Zustellung
- [x] net_tcp_recv: non-blocking + sock_block_thread() statt Busy-Wait
- [x] net_tcp_connect: registriert im Hash vor SYN, blockt bis SYN-ACK
- [x] net_tcp_accept: non-blocking + sock_block_thread()
- [x] net_tcp_close: sendet FIN, kehrt sofort zurueck (kein Busy-Wait)
- [x] udp recv: non-blocking + sock_block_thread()
- [x] net_idle() aus TCP/UDP entfernt (bleibt in ARP/DHCP/DNS fuer Boot)

## Net Phase E — Robustheit

- [ ] Out-of-Order Segment Buffering
- [ ] Slow-Start + AIMD Congestion Control
- [ ] Non-Blocking TCP Connect (EINPROGRESS)
- [ ] SO_RCVTIMEO / SO_SNDTIMEO pro Socket
- [ ] TCP Keepalive
- [ ] test/net/test_tcp_ooo.c
- [ ] test/net/test_net_multi_conn.c

## Net Tests — End-to-End (gegen echte Server)

- [ ] test/net/test_net_dns.c — DNS gegen QEMU slirp
- [ ] test/net/test_net_tcp_connect.c — TCP zu example.com:80
- [ ] test/net/test_net_tcp_transfer.c — HTTP GET, Body pruefen
- [ ] test/net/test_net_tls.c — HTTPS via Node.js
- [ ] test/net/test_net_npm_registry.c — HTTPS GET registry.npmjs.org

## Skalierung — Statische Arrays → Slab/Free-List/Hash

Problem: Statische Arrays mit O(n) Suche und festen Limits. Bei 1000+ Connections
unbenutzbar.

### Skal-A: Socket Pool (HOCH)

- [x] sockets[] (socket.c): Slab-Allokation statt static socket_t[64]
- [x] Free-List fuer O(1) Allokation/Freigabe
- [x] NET_MAX_SOCKETS 64→256 (Slab-Kapazitaet)
- [x] sock_alloc/sock_free anpassen
- [x] Active-List (doppelt verkettet) fuer Port-Conflict-Iteration
- [x] test: 128+ gleichzeitige Sockets (test_socket_scale.c)

### Skal-B: TCP Hash-Tabelle (HOCH)

- [x] tcp_hash[]: Chaining statt 1-Slot-per-Bucket (Linked-List pro Bucket)
- [x] Hash-Tabelle vergroessert (256 Buckets)
- [x] NET_TCP_MAX erhoehen (256)
- [x] test: 8 gleichzeitige TCP-Connections, Kollisions-Resilience, Unregister

### Skal-C: UDP Socket-Tabelle (MITTEL) ✓

- [x] udp_socks[]: Hash-Tabelle (64 Buckets, Chaining) + Slab-Pool (128 Slots)
- [x] NET_UDP_MAX → UDP_POOL_SIZE 128 (Slab-backed)
- [x] test: 32 gleichzeitige UDP-Sockets, rebind, hash-collision

### Skal-D: ARP Cache (MITTEL)

- [x] arp_cache[]: Hash-Tabelle (64 Buckets, Chaining) + Pool (128 Entries)
- [x] arp_cache_lookup() O(1) amortisiert, Round-Robin Eviction
- [x] test: 64 Eintraege, Kollision, Pool-Overflow (7 Tests)

### Skal-E: Timer-Wheel Pool (MITTEL)

- [x] tw_alloc/tw_free: O(1) Free-Stack statt O(n) lineare Suche
- [x] TW_MAX_TIMERS bereits 256
- [x] timer_wheel_active_count() O(1) Arithmetik statt O(n) Zaehlschleife
- [x] test: 128 Timer gleichzeitig, Cancel/Re-Alloc Roundtrip

### Skal-F: FD-Tabelle ✓

- [x] fd_alloc(): Free-Bitmap + __builtin_ctzll fuer O(1) + POSIX lowest-fd
- [x] FD_MAX erhoehen (256 → 1024)
- [x] test: 512+ offene FDs, lowest-free POSIX, EMFILE (test_fd_scale.c)

### Skal-G: PTY Pool

- [ ] pty_alloc(): O(4) mit Lock/Iteration → Bitmap oder Free-Stack O(1)
- [ ] Single Lock statt Lock-per-Slot
- [ ] PTY_MAX erhoehen (4 → 16+)
- [ ] test: 8+ PTYs gleichzeitig

### Skal-H: epoll_ctl Lookup

- [ ] epoll_ctl ADD/MOD/DEL: 3×O(64) linearer Scan → FD-Hash pro epoll O(1)
- [ ] EPOLL_MAX_FDS erhoehen (64 → 256+)
- [ ] test: epoll mit 128+ FDs

### Skal-I: Unix Socket Pool

- [ ] usock_alloc(): O(32) + Byte-Loop-Zeromem → Slab + memset O(1)
- [ ] USOCK_MAX erhoehen (32 → 128)
- [ ] test: 64+ AF_UNIX Sockets

### Skal-J: IPC Endpoint Pool

- [ ] ipc_create_endpoint(): O(64) → Free-List O(1)
- [ ] IPC_MAX_ENDPOINTS erhoehen (64 → 256)

### Skal-K: procfs FD Pool

- [ ] procfs_fd_alloc(): O(32) → Bitmap + ctz O(1)
- [ ] PROCFS_FD_MAX erhoehen (32 → 64)

### Skal-L: epoll Sleeper Array

- [ ] epoll_sleepers: Array[32] → Linked-List (kein Zeroout unter Lock)
- [ ] EPOLL_SLEEPER_MAX eliminieren (dynamisch)

### Skal-M: Process-Limit

- [ ] PROC_MAX=16 → dynamisch (Slab fuer process_t)
- [ ] pid_table[] dynamisch oder groesser (256 → 1024)
- [ ] Process-Group Hash (pgid → Prozessliste) fuer kill_pgrp O(1)

### Skal-N: Event-Pool Limits

- [ ] EPOLL_POOL_MAX=16 → Slab
- [ ] TIMERFD_POOL_MAX=16 → Slab
- [ ] INOTIFY_POOL_MAX=16 → Slab
- [ ] timerfd_any_expired(): O(n) → Min-Heap oder nearest-expire Tracking

## VT — Font-Rendering optimieren

### Font-A: Zweistufiger Glyph-Lookup

Problem: cp_to_glyph() macht binaere Suche ueber 1546 Eintraege bei jedem Zeichen.
99% der Terminal-Ausgabe ist ASCII/Latin-1 (U+0000–U+00FF).

- [ ] glyph_fast[256]: Direct-Map Array fuer U+0000–U+00FF → O(1), zero Branches
- [ ] cp_to_glyph_slow(): binaere Suche nur fuer U+0100+ (Cold-Path)
- [ ] cp_to_glyph(): if (cp < 256) return glyph_fast[cp], sonst slow
- [ ] glyph_fast[] beim Boot aus font_map[] befuellen
- [ ] test: ASCII Lookup O(1), Unicode Lookup korrekt

### Font-B: Font-Daten aus Kernel in initrd/ramfs

Problem: font_atlas.h = 12.391 Zeilen / ~200KB Glyph-Bitmaps im Kernel-Binary.
Font ist Userspace-Daten, nicht Kernel-Code.

- [ ] Font-Binary-Format definieren (Header + Glyph-Map + Bitmap-Daten)
- [ ] tools/mkfont.py: generiert .font Datei statt .h
- [ ] Font in ramfs einbetten: /lib/fonts/default.font
- [ ] fb_init(): laedt Font aus ramfs statt aus eingebettetem Header
- [ ] Abhaengigkeit: ramfs muss vor fb_init() verfuegbar sein (Boot-Reihenfolge pruefen)

## MM — Copy-on-Write fork()

- [x] PTE_COW (Bit 9): fork markiert User-Pages read-only + COW
- [x] Page-Fault-Handler: COW-Fault → neue Page bei refcount>1, PTE-Fix bei refcount==1
- [x] page_incref/page_decref/page_refcount (atomic, SMP-safe)
- [x] fork() O(Page-Tables) statt O(Speicher)
- [x] test: fork+write, fork-no-write, exit-refcount, multi-fork (16 Tests)

## MM — MADV_FREE (V8 Heap Management)

- [x] PTE_LAZYFREE (Bit 10): MADV_FREE markiert Pages, loescht Dirty-Bit
- [x] Erneuter Write → CPU setzt Dirty → Page gerettet (automatisch)
- [x] page_alloc() OOM → lazyfree_reclaim(): LAZYFREE+clean Pages freigeben
- [x] Demand-Paging: freigegebene Page → Zero-Page bei naechstem Zugriff
- [x] Fork: LAZYFREE-Bit gestrippt (COW hat Vorrang)
- [x] 24 Tests: basic, rewrite, multi-range, large, stress, fork, mprotect

## MM — Transparent Huge Pages (2MB)

- [x] huge_page_alloc/free: Order-9 Buddy-Allokation (512 Pages = 2MB)
- [x] PMD PS-Bit: Demand-Paging alloziert 2MB bei VMA_HUGEPAGE + 2MB-aligned
- [x] Automatisch: mmap(MAP_ANONYMOUS) >= 2MB → VMA_HUGEPAGE
- [x] Fallback auf 4KB bei Fragmentierung
- [x] COW-Split: fork zerlegt Huge Pages in 512×4KB COW-PTEs
- [x] split_huge_pmd(): munmap/mprotect/madvise auf Teilen splitten automatisch
- [x] 8 Tests: basic, 4mb, fallback, cow_split, partial_munmap, mprotect, stress, alignment

## VT — Terminal-Usability

### VT-A: Scrollback-Buffer

Problem: scroll_up() loescht vergangene Zeilen. cat bigfile → nur letzte N Zeilen
sichtbar, kein Zurueckscrollen.

- [ ] Scrollback-Ringbuffer (z.B. 4096 Zeilen) pro VT
- [ ] scroll_up: alte Zeilen in Scrollback schieben statt loeschen
- [ ] Shift+PageUp/PageDown: Scrollback navigieren
- [ ] Scroll-Position Reset bei neuer Ausgabe

### VT-B: Dirty-Line Tracking

Problem: mark_all_dirty() bei jedem Scroll → Full-Screen Framebuffer-Repaint.
cat von 1000 Zeilen = 1000 Full-Repaints.

- [ ] Dirty-Bitmap pro Zeile statt pro Screen
- [ ] fb_flush(): nur dirty Zeilen rendern
- [ ] scroll_up: nur neue untere Zeile als dirty markieren (Framebuffer-Scroll via memcpy)

### VT-C: Keymaps + AltGr

Problem: Hardcoded US-QWERTY. Kein QWERTZ, kein AltGr, keine Umlaute.
@ { } [ ] | \ ~ gehen auf DE-Layout nicht. Blocker fuer deutsche Nutzer.

- [ ] Keymap-Binaerformat: Header + 3×256 uint32_t (normal, shifted, altgr) = 3KB
- [ ] tools/mkkeymap.py: generiert .keymap Dateien
- [ ] Keymaps auf CosmoFS: /lib/keymaps/us.keymap, de.keymap, fr.keymap, ...
- [ ] Kernel laedt Keymap aus /lib/keymaps/ beim Boot (config: keymap = de)
- [ ] AltGr-Erkennung: Right-Alt als Level-3-Modifier
- [ ] Fallback: eingebettete US-Minimal-Keymap wenn /lib/keymaps/ nicht verfuegbar
- [ ] Dead-Keys fuer Akzente (optional, niedrige Prio)
- [ ] test: AltGr+Q → @ auf DE-Layout

### VT-D: Alternate Screen

Problem: less, vim, htop brauchen CSI ?1049h/l (Alternate Screen Buffer).
Ohne das ueberschreiben sie den normalen Screen.

- [ ] Zweiter Screen-Buffer pro VT (chars_alt, attrs_alt)
- [ ] CSI ?1049h: Wechsel zu Alternate Screen, Cursor save
- [ ] CSI ?1049l: Zurueck zum normalen Screen, Cursor restore
- [ ] test: Alternate Screen Switch + Restore

## Boot-Config — /etc/cosmo.conf

Problem: Keine Konfiguration. Keymap, VT-Farben, Startup-Apps, Netzwerk
sind hardcoded oder gar nicht konfigurierbar.

Zwei Quellen: Kernel Command-Line (Recovery) + /etc/cosmo.conf (normal).
Command-Line ueberschreibt Config-Datei.

- [ ] config_parse.h: config_load(path), config_get(key), config_get_int(key, default)
- [ ] Parser: key = value Format, # Kommentare, ~100 Zeilen
- [ ] Statisches Entry-Array (64 Eintraege)
- [ ] Boot-Reihenfolge: VFS init → CosmoFS mount → config_load → Keymap → VT → Netzwerk
- [ ] Unterstuetzte Keys:
      keymap (de/us/fr/...), vt.color (amber/green/white),
      vt.font (/lib/fonts/...), vt.scrollback (Zeilen),
      vt.0..vt.3 (Startup-Programm pro VT),
      net.hostname, net.dns,
      smp.cores (auto/N)
- [ ] Kernel Command-Line Parsing aus UEFI Boot Services (Fallback)
- [ ] test: config_get nach config_load

## Job Control — bash/Shell-Interaktion

Problem: Ctrl-C/Ctrl-Z in bash funktionieren nicht. Ohne Job Control ist
eine interaktive Shell kaum benutzbar. Blocker fuer Self-Hosting.

- [ ] TIOCSPGRP/TIOCGPGRP ioctl: Foreground Process Group pro TTY setzen/lesen
- [ ] SIGTSTP (Ctrl-Z): Foreground-Prozess stoppen (TASK_STOPPED State)
- [ ] SIGCONT: Gestoppten Prozess fortsetzen (fg/bg Kommandos)
- [ ] SIGINT (Ctrl-C): An Foreground Process Group senden (nicht nur PID 1)
- [ ] SIGQUIT (Ctrl-\): An Foreground Process Group senden
- [ ] Terminal Driver: Ctrl-C/Z/\ in PTY erkennen → Signal an Foreground PG
- [ ] waitpid WUNTRACED/WCONTINUED: bash muss gestoppte Kinder erkennen
- [ ] test: Ctrl-C killt Foreground-Prozess, nicht Shell
- [ ] test: Ctrl-Z stoppt Prozess, fg setzt fort

## Self-Hosting Blocker — Terminal-OS mit Homebrew

### SH-A: Dynamic Linker (KRITISCH)

Problem: Brew-Packages sind dynamisch gelinkt. Ohne ld-cosmo.so startet
nichts aus Homebrew (git, gcc, vim, grep, ...).

- [ ] ld-cosmo.so: ELF PT_INTERP, .dynamic Section, Relocation
- [ ] DT_NEEDED: Shared Libraries laden (libc.so, libm.so, ...)
- [ ] Symbol Resolution: PLT/GOT lazy binding
- [ ] RPATH/RUNPATH fuer Brew-Prefix (/home/linuxbrew/.linuxbrew/lib)
- [ ] test: dynamisch gelinktes Hello World
- [ ] test: Brew-installiertes git startet

### SH-B: Device Nodes

- [ ] /dev/null: open → fd, write → discard, read → EOF
- [ ] /dev/zero: read → zero-bytes, mmap → zero-pages
- [ ] /dev/urandom: read → getrandom() Bytes
- [ ] /dev/tty: Alias fuer controlling Terminal
- [ ] VFS: Spezial-Inodes fuer Character-Devices (S_IFCHR)

### SH-C: MAP_SHARED mmap

Problem: git, gcc, make nutzen shared memory-mapped Files.
Aktuell nur MAP_PRIVATE.

- [ ] MAP_SHARED: aendern am mmap-Bereich schreibt in die Datei
- [ ] msync: Flush von shared Mappings
- [ ] Mehrere Prozesse koennen gleiche Datei MAP_SHARED mappen
- [ ] test: MAP_SHARED write + read von zweitem Prozess

### SH-D: Signale vervollstaendigen

- [ ] SIGPIPE: write auf geschlossene Pipe/Socket → SIGPIPE an Writer
- [ ] SIGALRM: alarm() / setitimer() Syscall
- [ ] SIGCHLD: korrekt an Parent bei Child-Exit (fuer wait-Loops)
- [ ] SIGWINCH: Terminal-Resize an Foreground-Prozess
- [ ] test: write auf geschlossene Pipe → SIGPIPE
- [ ] test: alarm(1) → SIGALRM nach 1s

### SH-E: Terminal-Groesse

- [ ] ioctl TIOCGWINSZ: Terminal-Breite/Hoehe abfragen (ncurses, vim, htop)
- [ ] ioctl TIOCSWINSZ: Terminal-Groesse setzen (bei Resize)
- [ ] SIGWINCH bei Groessenaenderung an Foreground Process Group
- [ ] struct winsize in PTY speichern
- [ ] test: TIOCGWINSZ liefert korrekte Spalten/Zeilen

### SH-F: /proc erweitern

- [ ] /proc/self/exe: Symlink auf eigene ELF-Binary (Node.js braucht das)
- [ ] /proc/pid/cmdline: Kommandozeile des Prozesses (ps, htop)
- [ ] /proc/pid/stat: Prozess-Status (ps)
- [ ] /proc/pid/maps: Memory Mappings (Debugging)
- [ ] /proc/meminfo: Freier/Belegter Speicher

### SH-G: Locale + Timezone

- [ ] TZ Environment-Variable: UTC Offset berechnen
- [ ] /etc/localtime: Timezone-Datei (Symlink auf /usr/share/zoneinfo/...)
- [ ] LC_ALL/LANG: UTF-8 Locale (CosmoPX libc)
- [ ] test: time() mit korrektem TZ Offset

### SH-H: Symlinks + Permissions

- [ ] Symlink-Resolution in allen Pfad-Operationen (open, stat, exec, ...)
- [ ] Brew Symlink-Farm: /usr/local/bin/git → ../Cellar/git/2.x/bin/git
- [ ] CosmoFS Inode: rwx Bits speichern (chmod aendert, stat liefert)
- [ ] Enforcement: nur +x wird geprueft (exec). r/w gespeichert aber nicht enforced (Single-User)
- [ ] Default: Dirs 0755, Files 0644, nach chmod beliebig
- [ ] test: Symlink-Chain resolution (3 Ebenen)
- [ ] test: chmod +x, dann exec
- [ ] test: stat liefert gesetzte Permissions korrekt zurueck

## Offen

- [ ] c-ares UDP DNS (c-ares ETIMEOUT trotz korrekter Pakete)
- [ ] GPT-Image Boot (Partitions-Support)
