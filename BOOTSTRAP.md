# CosmoRT Bootstrap: Claude Code Boot Image

Anleitung fuer den CosmoRT-Kernel-Entwickler.

Ziel: QEMU-Image das direkt in Claude Code bootet.
Kein Init-System, kein Login. Kernel → init → claude.

## Kontext: Was ist CosmoPX?

CosmoPX (Repo: ~/Git/CosmoPX) ist eine from-scratch POSIX-Plattform:
libc, Shell, Coreutils, Toolchain, Package-Manager. Alles in C11,
kein Code aus glibc/musl/busybox. ~132.000 Zeilen, 413 Tests.

Der gesamte Stack ist darauf ausgelegt, Claude Code auf einem
eigenen OS laufen zu lassen — damit der Agent das OS weiterentwickelt,
auf dem er selbst laeuft (Meilenstein 7: Selbstverstaerkung).

### Was existiert (Stand 2026-03-21)

```
CosmoPX/
  libc/           POSIX libc from scratch
                  ~15.400 Zeilen C, 108 Header, ~1000 Symbole
                  141 Tests (musl libc-test Suite)
                  IEEE 1003.1-2024, pthreads, DNS-Resolver, regex
                  Thread-safe malloc (size-class-segregated, mmap-backed)
                  Thread-safe stdio (FILE-Locking via pthread_mutex)

  sh/             POSIX Shell from scratch
                  ~5000 Zeilen, 42 Tests
                  Builtins: cd echo test exit export set shift read eval
                  exec trap printf getopts command type local return break
                  continue source true false : umask kill readonly times
                  Kann configure-Scripts ausfuehren (zlib, gmp, openssl, ...)

  coreutils/      47 POSIX Tools from scratch
                  104 Tests
                  cat cp mv rm ln chmod mkdir find grep sed awk sort
                  diff tr cut xargs mktemp tar gzip paste split
                  mkfifo getconf ... (vollstaendige Liste in Abschnitt 2)

  buildutils/     POSIX make from scratch
                  ~3000 Zeilen, 40 Tests
                  Pattern Rules, GNU-Funktionen, Conditionals

  fileutils/      tar + gzip/DEFLATE from scratch
                  17 Tests, Path-Traversal-geschuetzt

  toolchain/      GCC Specs-File + eigene ar/nm/strip/ranlib
                  69 Tests, Sysroot mit libc.a + CRT + Header

  brew/           CosmoBrew Package-Manager (Ruby)
                  Bootstrap: 14 Pakete aus Quellcode gebaut
                  zlib gmp mpfr mpc binutils gcc openssl ncurses
                  readline libffi libyaml ruby bash git node
```

### Was validiert ist

```
Meilenstein 1 ✓  libc-test komplett (141/141)
Meilenstein 2 ✓  Userland from scratch (sh + 47 coreutils + make + tar)
Meilenstein 3 ✓  Toolchain (Self-Hosted, 69/69 Tests)
Meilenstein 4 ✓  Bootstrap Stage 1+2 (14 Pakete aus Quellcode)
Meilenstein 5 ✓  Ruby + CosmoBrew PM
Meilenstein 6 ✓  Node.js 22.14 + Claude Code 2.1.81

Validiert:  node --version                → v22.14.0
            node -e "typeof Intl"         → object  (ICU 76.1)
            claude --version              → 2.1.81 (Claude Code)
```

Alle Binaries sind statisch gelinkt. Kein glibc, kein ld-linux.
`nm -u binary` zeigt keine unaufgeloesten Symbole.

### Security-Audit (durchgefuehrt 2026-03-21)

Alle kritischen Findings gefixt:

- tar: Path-Traversal-Validierung + Symlink-Sanitisierung
- malloc: size_t statt unsigned int (>4GB safe)
- malloc: posix_memalign Overflow-Check
- DNS: Label-Decompression Bounds-Check
- vsnprintf: Width/Precision-Cap gegen Integer-Overflow
- glob: Rekursionslimit gegen exponentielles Backtracking
- stdio: FILE-Locking via pthread_mutex (thread-safe)
- malloc: globaler Mutex auf Free-Lists (thread-safe)

### Performance-Audit (durchgefuehrt 2026-03-21)

Optimierungen eingebaut:

- memcpy/memset: word-at-a-time statt byte-at-a-time (~8x)
- strcspn/strpbrk: 256-Byte Lookup-Table statt O(n*m)
- realloc: mremap vor malloc+copy+free
- Shell-Variablen: Hash-Table statt Linked-List
- make: Rekursive Variablen gecacht (Generation-Counter)
- make: $(shell) Ergebnisse gecacht
- fflush(NULL): nur dirty Files statt alle offenen

### Was der Kernel liefern muss

CosmoPX liefert alles oberhalb der Syscall-Schicht:
libc, Tools, Node.js, Claude Code. Alles statisch.

CosmoRT muss liefern:
- ~80 Syscalls (siehe Abschnitt 4, nach Prioritaet sortiert)
- Virtuelle Dateisysteme: procfs, sysfs, devtmpfs, devpts, tmpfs
- Netzwerk: IPv4 TCP/UDP Stack (fuer HTTPS zu api.anthropic.com)
- PTY-Subsystem (Claude Code spawnt Shells)

Der Rest dieses Dokuments spezifiziert exakt was gebraucht wird.

## Architektur

```
┌─────────────────────────────────────────────┐
│  Claude Code 2.1.81                         │
│  /opt/claude-code/cli.js                    │
├─────────────────────────────────────────────┤
│  Node.js 22.14.0 (statisch, ICU 76.1)      │
│  /usr/bin/node  — 73 MB stripped            │
├─────────────────────────────────────────────┤
│  CosmoCL Userland                           │
│  sh, 47 coreutils, git, bash                │
│  alle statisch gelinkt, kein glibc          │
├─────────────────────────────────────────────┤
│  CosmoCL libc  — ~1000 Symbole              │
│  POSIX libc, pthreads, DNS, stdio, malloc   │
│  statisch in jede Binary eingelinkt         │
├─────────────────────────────────────────────┤
│  CosmoRT Kernel                             │
│  Syscalls (siehe Abschnitt 4)               │
└─────────────────────────────────────────────┘
```

## 1. Boot-Ablauf

```
CosmoRT Kernel
  → /init
    → mount /proc, /sys, /dev, /tmp, /dev/pts
    → Netzwerk (DHCP oder statisch, DNS)
    → ANTHROPIC_API_KEY aus Kernel-Cmdline oder Environment
    → exec node /opt/claude-code/cli.js
```

Referenz-Init:

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
mkdir -p /dev/pts /tmp
mount -t devpts devpts /dev/pts
mount -t tmpfs tmp /tmp
hostname cosmo

# Netzwerk (QEMU user-mode: Gateway 10.0.2.2, DNS 10.0.2.3)
ip link set lo up
ip link set eth0 up
udhcpc -i eth0 2>/dev/null || {
    ip addr add 10.0.2.15/24 dev eth0
    ip route add default via 10.0.2.2
}
echo "nameserver 10.0.2.3" > /etc/resolv.conf

export HOME=/home/cosmo
export PATH=/usr/bin:/bin
export TERM=xterm-256color
# API-Key aus Kernel-Cmdline extrahieren:
export ANTHROPIC_API_KEY=$(cat /proc/cmdline | tr ' ' '\n' | grep ^apikey= | cut -d= -f2-)

cd "$HOME"
exec node /opt/claude-code/cli.js
```

## 2. Image-Inhalt

### Binaries (alle statisch, kein glibc)

```
/usr/bin/
  node              73 MB  (stripped, --with-intl=small-icu)
  sh                        CosmoCL POSIX Shell
  bash                      fuer configure-Scripts
  git                       Kernfunktion von Claude Code
  env id uname which ls cat cp mv rm mkdir rmdir chmod
  touch head tail wc tee sleep pwd basename dirname
  find grep sed awk diff sort uniq cut tr xargs
  mktemp readlink realpath expr date yes printf test
  ln install mkfifo paste split getconf
  tar gzip gunzip zcat
  npm npx                  → Symlinks auf node-Wrapper
```

Claude Code spawnt diese Tools direkt. Fehlende Binaries
fuehren zu degradierter Funktionalitaet, nicht zum Crash.

### Node Modules

```
/opt/claude-code/           73 MB
  cli.js                    Einstiegspunkt
  node_modules/
  vendor/
  resvg.wasm

/opt/npm/                   24 MB  (optional)
  lib/node_modules/npm/
```

### Dateisystem-Skelett

```
/proc                       procfs
/sys                        sysfs
/dev
  null zero urandom         Pflicht
  tty                       Pflicht
  ptmx + pts/               PTY (Claude Code spawnt Shells)
/tmp                        tmpfs
/etc
  resolv.conf               nameserver 8.8.8.8  (oder QEMU 10.0.2.3)
  hosts                     127.0.0.1 localhost
  passwd                    root:x:0:0::/root:/bin/sh
/home/cosmo                 Arbeitsverzeichnis
```

### Image-Groesse

```
node (stripped)          73 MB
claude-code              73 MB
npm                      24 MB
coreutils + sh + git     18 MB
/etc + /dev skeleton      < 1 MB
────────────────────────────────
Gesamt                  ~190 MB unkomprimiert
                         ~60 MB squashfs/gzip
```

512 MB RAM reicht. Node/V8 braucht ~200 MB, Rest fuer Tools.

### Wie Node.js gebaut wurde

Node.js 22.14.0 wird im CosmoPX-Bootstrap aus Quellcode gebaut:

```sh
cd brew/src/node-v22.14.0
./configure \
    --fully-static \
    --dest-os=linux \
    --with-intl=small-icu \
    --without-inspector \
    --without-node-snapshot \
    --prefix=$BREW_PREFIX
make -j$(nproc)
make install
```

Kritische Flags:
- `--fully-static` — keine Shared Libraries, kein ld-linux
- `--with-intl=small-icu` — PFLICHT. Claude Code nutzt Unicode
  Property Escapes in Regex (`\p{Default_Ignorable_Code_Point}`).
  Ohne ICU crasht Claude Code beim Import mit:
  `Invalid regular expression: /^\p{...}$/u: Invalid property name`
  `--without-intl` ist NICHT ausreichend.
- `--without-inspector` — Chrome DevTools nicht noetig
- `--without-node-snapshot` — reduziert Binary-Groesse

Resultat: 73 MB statische Binary mit ICU 76.1, Unicode 16.0.
Intl-Objekt vorhanden, Segmenter vorhanden, \p{} Regex funktioniert.

### Welche CosmoCL-libc-Features Node.js braucht

Node.js/V8/libuv nutzen folgende libc-Subsysteme intensiv:

```
pthreads        V8 Worker-Threads, libuv Thread-Pool (4 Threads default)
                pthread_create, pthread_join, pthread_mutex_*, pthread_cond_*
futex           Mutex/Condvar-Implementation (Low-Level)
mmap            V8 Heap, JIT-Code-Pages (mmap + mprotect RWX→RX)
epoll           libuv Event-Loop (ALLE async I/O geht hierueber)
pipe2           Parent↔Child Kommunikation
fork+execve     Tool-Execution (Claude Code spawnt sh, git, grep, ...)
socket          HTTPS zu api.anthropic.com, Unix-Sockets fuer IPC
getaddrinfo     DNS-Aufloesung (CosmoCL: /etc/hosts + RFC 1035 UDP)
clock_gettime   Timestamps, Performance-Messung
getrandom       V8 braucht Entropy fuer ASLR, Math.random(), Crypto
arch_prctl      TLS-Setup (FS-Register) — ohne das crasht errno-Zugriff
```

## 3. Netzwerk

Claude Code braucht HTTPS zu api.anthropic.com (Port 443).
TLS ist in Node.js eingebaut (BoringSSL/OpenSSL im Binary).

Minimale Anforderungen:
- IPv4 Stack (AF_INET, SOCK_STREAM, SOCK_DGRAM)
- DNS-Aufloesung funktioniert (CosmoCL getaddrinfo: /etc/hosts + UDP/53)
- TCP-Verbindung zu Port 443
- Kein HTTP-Proxy noetig (aber NODE_EXTRA_CA_CERTS falls eigene CA)

QEMU User-Mode Networking (`-nic user`) reicht fuer Entwicklung.
Produktiv: virtio-net + eigener DHCP-Client oder statische Konfig.

## 4. Syscall-Anforderungen an CosmoRT

Alles was Node.js 22.14 + Claude Code + Userland braucht.
Getestet auf x86_64. Syscall-Nummern fuer diese Architektur.

### Tier 1 — Ohne diese startet nichts

```
Datei-I/O:
  read(0) write(1) open(2) close(3) stat(4) fstat(5) lstat(6)
  poll(7) lseek(8) access(21) pipe(22) pipe2(293)
  dup(32) dup2(33) dup3(292)
  openat(257) newfstatat(262) unlinkat(263) renameat(264)
  faccessat(269) readlinkat(267) fchmodat(268) fchownat(260)
  mkdirat(258) mknodat(259)
  getdents64(217) getcwd(79) chdir(80)

Speicher:
  mmap(9) mprotect(10) munmap(11) brk(12) mremap(25) madvise(28)

Prozesse:
  clone(56) fork(57) vfork(58) execve(59)
  wait4(61) exit(60) exit_group(231)
  getpid(39) getppid(110) getuid(102) getgid(104) geteuid(107) getegid(108)
  setsid(112) setpgid(109) getpgid(121)
  kill(62) tgkill(234)

Signale:
  rt_sigaction(13) rt_sigprocmask(14) rt_sigreturn(15) rt_sigsuspend(130)
  sigaltstack(131)

Zeit:
  clock_gettime(228) clock_getres(229) nanosleep(35)
  gettimeofday(96)

Misc:
  ioctl(16) fcntl(72) uname(63) sysinfo(99)
  getrandom(318) prctl(157)
  getrlimit(97) prlimit64(302)
  arch_prctl(158)           FS/GS Register fuer TLS — kritisch
  set_tid_address(218)
  set_robust_list(273)
```

### Tier 2 — Node.js I/O-Loop (libuv)

```
epoll_create1(291) epoll_ctl(233) epoll_wait(232)
  → V8/libuv Event-Loop. Ohne epoll startet Node nicht.

eventfd2(290)
  → Thread-Wakeup in libuv

timerfd_create(283) timerfd_settime(286)
  → Timer in libuv (optional, Fallback auf nanosleep)

inotify_init1(294) inotify_add_watch(254) inotify_rm_watch(255)
  → Filesystem-Watcher. Claude Code nutzt das fuer File-Monitoring.
  → Ohne inotify: degradiert, kein Crash.
```

### Tier 3 — Netzwerk

```
socket(41) connect(42) bind(49) listen(50) accept4(288)
sendto(44) recvfrom(45) sendmsg(46) recvmsg(47)
setsockopt(54) getsockopt(55)
getpeername(52) getsockname(51)
shutdown(48)

Unterstuetzte Familien:
  AF_UNIX  (1)   — Node IPC, Claude Code ↔ MCP Server
  AF_INET  (2)   — IPv4, API-Zugang
  AF_INET6 (10)  — IPv6 (optional aber empfohlen)

Unterstuetzte Typen:
  SOCK_STREAM     — TCP
  SOCK_DGRAM      — UDP (DNS)
  SOCK_NONBLOCK   — Flag, nicht eigener Typ
  SOCK_CLOEXEC    — Flag
```

### Tier 4 — PTY (Claude Code spawnt Shells)

```
openat(/dev/ptmx)
  → Pseudo-Terminal Master

ioctl:
  TIOCGPTN (0x80045430)     — PTY-Nummer abfragen
  TIOCSPTLCK (0x40045431)   — PTY entsperren
  TIOCSWINSZ (0x5414)       — Fenstergroesse setzen
  TIOCGWINSZ (0x5413)       — Fenstergroesse abfragen
  TIOCSPGRP  (0x5410)       — Vordergrund-Prozessgruppe
  TIOCGPGRP  (0x540F)       — dito, lesen

/dev/pts/ muss als devpts gemountet sein.
Ohne PTY: Claude Code kann keine Shell-Befehle ausfuehren.
```

### Tier 5 — Filesystem-Features

```
statfs(137) fstatfs(138)
  → Node prueft Filesystem-Typ

utimensat(280)
  → Timestamps setzen (touch, git)

symlink(88) symlinkat(266) link(86) linkat(265)
  → npm nutzt Symlinks extensiv

chmod(90) fchmod(91) chown(92) fchown(93)
  → Dateiberechtigungen

truncate(76) ftruncate(77)
  → Dateien kuerzen
```

## 5. Was NICHT gebraucht wird

```
Kein brk/sbrk          — malloc ist mmap-basiert
Kein futex_waitv        — Standard-futex reicht
Kein io_uring           — libuv nutzt epoll
Kein seccomp            — kein Sandboxing noetig
Kein clone3             — clone() reicht
Kein cgroups            — kein Container-Support
Kein netlink            — kein Netzwerk-Management zur Laufzeit
Kein NUMA               — Single-Socket Annahme
Kein audit/perf/bpf     — kein Tracing
Kein mount namespace     — flaches Dateisystem
```

## 6. Validierung

### Schritt 1: Node startet

```sh
qemu$ node -e "console.log('hello from CosmoRT')"
# Erwartet: hello from CosmoRT
# Testet: mmap, brk, clone (V8 Worker), epoll, getrandom, arch_prctl
```

### Schritt 2: Node I/O funktioniert

```sh
qemu$ node -e "require('fs').writeFileSync('/tmp/test', 'ok'); console.log(require('fs').readFileSync('/tmp/test', 'utf8'))"
# Erwartet: ok
# Testet: open, write, read, close, stat
```

### Schritt 3: Netzwerk funktioniert

```sh
qemu$ node -e "require('https').get('https://api.anthropic.com', r => { console.log(r.statusCode); process.exit() })"
# Erwartet: 401 (kein API-Key, aber TCP+TLS+DNS funktionieren)
# Testet: socket, connect, DNS (getaddrinfo → UDP/53), TLS Handshake
```

### Schritt 4: PTY funktioniert

```sh
qemu$ node -e "const {execSync} = require('child_process'); console.log(execSync('echo ok').toString().trim())"
# Erwartet: ok
# Testet: fork, execve, pipe, wait4
```

### Schritt 5: Claude Code startet

```sh
qemu$ ANTHROPIC_API_KEY=sk-... node /opt/claude-code/cli.js --version
# Erwartet: 2.1.81 (Claude Code)
# Testet: alles zusammen
```

### Schritt 6: Claude Code arbeitet

```sh
qemu$ ANTHROPIC_API_KEY=sk-... node /opt/claude-code/cli.js -p "echo hello"
# Testet: HTTPS API-Call, Tool Execution, PTY, File I/O
```

## 7. QEMU-Kommando

```sh
qemu-system-x86_64 \
    -kernel cosmo-bzImage \
    -initrd cosmo-initrd.img \
    -append "console=ttyS0 quiet apikey=sk-ant-..." \
    -m 512M \
    -smp 2 \
    -nic user,model=virtio \
    -nographic \
    -serial mon:stdio
```

Optionen:
- `-m 512M` reicht. Node/V8 ~200 MB, Tools ~50 MB, Rest fuer Dateien.
- `-smp 2` minimum. V8 nutzt Worker-Threads.
- `-nic user` fuer NAT-Netzwerk ueber Host. Gateway 10.0.2.2, DNS 10.0.2.3.
- `-nographic -serial mon:stdio` fuer Terminal-Zugriff.

## 8. Kritischer Pfad

Reihenfolge der Implementation im Kernel:

```
1. Basis-Syscalls          → /bin/sh startet
   (read, write, open, close, mmap, fork, execve, wait4,
    getpid, arch_prctl, set_tid_address)

2. epoll + eventfd         → Node.js startet
   (epoll_create1, epoll_ctl, epoll_wait, eventfd2, getrandom)

3. Netzwerk-Stack          → HTTPS funktioniert
   (socket, connect, bind, sendto, recvfrom, setsockopt
    fuer AF_INET + SOCK_STREAM + SOCK_DGRAM)

4. PTY                     → Claude Code kann Tools ausfuehren
   (openat /dev/ptmx, ioctl TIOCGPTN/TIOCSPTLCK/TIOCSWINSZ,
    devpts Filesystem)

5. inotify                 → File-Watching (nice-to-have)
```

Jeder Schritt ist einzeln testbar. Nicht zum naechsten gehen
bevor der aktuelle Validierungsschritt (Abschnitt 6) besteht.

## 9. Was CosmoPX liefert (nicht im Kernel noetig)

Diese Funktionalitaet ist komplett in der libc / im Userland
implementiert. Der Kernel muss hier nichts tun:

```
DNS-Resolver        getaddrinfo() in libc, UDP-Sockets genuegen
TLS/HTTPS           in Node.js eingebaut (BoringSSL)
Buffered I/O        stdio in libc (fopen/fread/fprintf/...)
malloc/free         mmap-basiert, thread-safe, kein brk/sbrk
pthreads            1:1 Kernel-Threads via clone() + futex
regex               NFA-Engine in libc
Dateikompression    DEFLATE/gzip in fileutils
Build-System        make in buildutils
Paketverwaltung     CosmoBrew (Ruby)
```

Der Kernel muss nur die Syscall-Schicht bereitstellen.
Alles darueber kommt aus CosmoPX.

## 10. Quellen

```
CosmoPX Repo:     ~/Git/CosmoPX
CosmoPX CLAUDE.md: vollstaendige Projektdokumentation
Node.js Source:   ~/Git/CosmoPX/brew/src/node-v22.14.0/
Binaries:         ~/Git/CosmoPX/brew/prefix/bin/
Claude Code:      ~/Git/CosmoPX/brew/prefix/lib/node_modules/@anthropic-ai/claude-code/
```
