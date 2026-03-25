# CosmoRT — Offene Punkte

Stand: 2026-03-25. 579 ktest PASS, 0 FAIL.
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

### RT/Compute-G: SIMD Hash-Engine (RT-Core Idle Background-Optimizer)

RT-Core ist >98% idle. Nutze die Idle-Zeit fuer Background-Hashing mit
SSE2/SHA-NI. Kein fxsave noetig — RT-Core hat keinen Userspace-FPU-State.

Eine Hash-Engine, drei Konsumenten: RAM-Dedup, FS-Dedup, Cloud-Sync.

#### G1: SIMD SHA-256 auf RT-Core

- [ ] src/arch/x86_64/sha256_sse.asm: SHA-256 mit SSE2 (Fallback) + SHA-NI (wenn verfuegbar)
- [ ] CPUID-Check fuer SHA-NI Support (Zen+, Goldmont+)
- [ ] Eigene CFLAGS (-msse2/-msha) fuer Hash-Datei, nicht global
- [ ] rt_poll P7 (niedrigste): hash_poll() verarbeitet Jobs im Idle
- [ ] rt_hash_request(addr, len, callback) — Compute postet Hash-Job via rt_channel
- [ ] test: SHA-256 korrekt (Testvektoren aus NIST)
- [ ] test: Hash-Durchsatz (Bytes/s)

#### G2: RAM-Dedup (KSM auf RT-Core)

MM↔Dedup Interface: MM kennt kein Hashing, Dedup kennt keine PTEs.

MM exportiert:
- [ ] mm_dedup_scan_next(phys_out): naechste anonyme User-Page (refcount==1, nicht gelockt)
- [ ] mm_dedup_scan_reset(): Scan-Zyklus neu starten
- [ ] mm_dedup_merge(keep, victim): alle PTEs victim→keep umschreiben (COW), page_decref
- [ ] include/kernel/mm/dedup.h: Interface-Header

Dedup exportiert (Lifecycle-Callbacks fuer MM):
- [ ] dedup_on_page_free(phys): Hash aus Tabelle entfernen
- [ ] dedup_on_cow_break(old, new): alten Hash invalidieren, neuen queuen
- [ ] page_free() und COW-Fault-Handler rufen Callbacks auf

Page-Age Tracking (Accessed/Dirty Hardware-Bits):
- [ ] include/kernel/mm/page_age.h: PAGE_HOT/WARM/COLD enum
- [ ] 2-Bit Age-Counter pro physische Page (256KB fuer 4GB RAM)
- [ ] mm_age_pages(): periodisch Accessed-Bit lesen, Age shiften, Accessed clearen
- [ ] mm_page_temperature(phys): HOT/WARM/COLD abfragen
- [ ] Dedup scannt nur PAGE_COLD + nicht-dirty Pages (kein Waste auf hot Pages)

Dedup-Engine:
- [ ] Hash-Tabelle: hash[32] → phys_addr (statisch, 4096 Buckets)
- [ ] hash_poll(): scan_next → skip hot → sha256 → lookup → memcmp → merge
- [ ] Scan-Rate begrenzt (max N Pages pro rt_poll Durchlauf)
- [ ] Statistik: /proc/ksm (pages_scanned, pages_shared, pages_merged, bytes_saved)
- [ ] test: zwei Prozesse mit identischen Pages → nach Merge nur 1 physische Page
- [ ] test: hot Page wird uebersprungen, cold Page wird gemerged
- [ ] test: COW-Break nach Merge → neue Page, alter Hash invalidiert

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

- [ ] sockets[] (socket.c): Slab-Allokation statt static socket_t[64]
- [ ] Free-List fuer O(1) Allokation/Freigabe
- [ ] NET_MAX_SOCKETS erhoehen oder eliminieren (Slab-Limit)
- [ ] sock_alloc/sock_free anpassen
- [ ] test: 128+ gleichzeitige Sockets

### Skal-B: TCP Hash-Tabelle (HOCH)

- [x] tcp_hash[]: Chaining statt 1-Slot-per-Bucket (Linked-List pro Bucket)
- [x] Hash-Tabelle vergroessert (256 Buckets)
- [x] NET_TCP_MAX erhoehen (256)
- [x] test: 8 gleichzeitige TCP-Connections, Kollisions-Resilience, Unregister

### Skal-C: UDP Socket-Tabelle (MITTEL)

- [ ] udp_socks[]: Hash-Tabelle (Port → Socket) statt lineares Array[16]
- [ ] NET_UDP_MAX erhoehen oder dynamisch
- [ ] test: 32+ gleichzeitige UDP-Sockets

### Skal-D: ARP Cache (MITTEL)

- [ ] arp_cache[]: Hash-Tabelle (IP → MAC) statt lineares Array[16]
- [ ] test: 32+ ARP-Eintraege ohne Performance-Degradation

### Skal-E: Timer-Wheel Pool (MITTEL)

- [ ] tw.entries[]: Free-List oder Bitmap statt O(n) lineare Suche
- [ ] TW_MAX_TIMERS erhoehen (64 → 256+)
- [ ] test: 128+ aktive Timer

### Skal-F: FD-Tabelle (NIEDRIG)

- [ ] fd_table.entries[]: Free-List Index fuer O(1) fd_alloc
- [ ] FD_MAX erhoehen (256 → 1024) oder dynamisch
- [ ] test: 512+ offene FDs pro Prozess

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

Problem: fork() deep-copied jede Seite (process.c:544-548). Node.js Worker-fork
bei 500MB Heap = 500MB kopiert, auch wenn Child nur 2MB aendert.

- [ ] COW-Bit im PTE: fork markiert alle User-Pages read-only in beiden Prozessen
- [ ] Page-Fault-Handler: Schreibzugriff auf COW-Page → neue Page allozieren, kopieren, PTE writable
- [ ] Refcount pro physische Page (page_alloc.h): fork incrementiert, munmap/exit decrementiert
- [ ] fork() wird O(Page-Tables) statt O(Speicher): nur PTEs kopieren, nicht Daten
- [ ] COW-sichere Kernel-Zugriffe: copy_from_user darf COW-Pages nicht triggern
- [ ] test: fork + child write → eigene Page, parent unveraendert
- [ ] test: fork ohne write → keine neuen Pages alloziert
- [ ] test: exit nach fork → refcount korrekt decrementiert

## MM — MADV_FREE (V8 Heap Management)

Problem: V8 gibt Heap-Seiten mit madvise(MADV_FREE) zurueck. Kernel darf sie bei
Speicherdruck recyclen, Prozess behaelt den VA-Range. Aktuell nur MADV_DONTNEED
(zerstoert sofort), V8 braucht lazy reclaim.

- [ ] VMA-Flag VM_LAZYFREE: Seiten als reclaimable markieren statt sofort freigeben
- [ ] Page-Reclaim: unter Speicherdruck LAZYFREE-Seiten zuerst freigeben
- [ ] Dirty-Check: LAZYFREE-Seite die erneut beschrieben wird verliert LAZYFREE-Status
- [ ] MADV_FREE in do_madvise (sys_mem.c) implementieren
- [ ] test: madvise(MADV_FREE) + erneuter Zugriff → Seite noch da (kein Druck)
- [ ] test: madvise(MADV_FREE) + Speicherdruck → Seite weg, erneuter Zugriff → Zero-Page

## MM — Transparent Huge Pages (2MB)

Problem: 500MB Node.js Heap = 128.000 4KB-PTEs = TLB-Thrashing.
2MB Huge Pages reduzieren auf ~250 PTEs.

- [ ] 2MB-Page-Allocator (Buddy oder freelist fuer order-9 Pages)
- [ ] PTE-Promotion: 512 zusammenhaengende 4KB-Pages mit gleichen Flags → 1 PMD-Entry (2MB)
- [ ] Automatische Promotion bei mmap(MAP_ANONYMOUS) >= 2MB-aligned
- [ ] Page-Fault auf 2MB-Page: direkt 2MB allozieren wenn alignment+size passen
- [ ] Fallback auf 4KB wenn 2MB nicht verfuegbar (keine Fragmentierung)
- [ ] COW-Interaktion: COW-Fault auf Huge Page → erst zu 4KB splitten, dann COW
- [ ] test: mmap 4MB aligned → 2 Huge Pages in PMD
- [ ] test: Huge Page + fork → COW split zu 4KB

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
