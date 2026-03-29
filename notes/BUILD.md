# Build-System Restructuring

## Problem

Ein Makefile (1000+ Zeilen) macht alles:
- Kernel Build (BOOTX64.EFI)
- Test Build (ktest, ktest-crash, ktest-fuzz)
- Alpine Image Build (ext2, disk.img)
- QEMU Targets (test-hw, qemu-alpine, qemu-disk)
- Tool Build (mkfs, embed.py)

Konflikte: `make test-hw` ueberschreibt init_bin.h → Alpine Image kaputt.
`make all` baut ktest statt init.c wenn test-hw vorher lief.

## Vorschlag

```
Makefile                    Kernel only. `make` → build/BOOTX64.EFI
test/Makefile               Tests. `make -C test hw` `make -C test crash`
src/arch/x86_64/Makefile    Arch-spezifisch (Boot, Trampoline, ASM)
```

### Root Makefile (minimal)

```makefile
# CosmoRT Kernel Build
include src/arch/x86_64/Makefile.arch   # KCFLAGS, ASM rules, boot

all: $(BUILD)/BOOTX64.EFI

# Kernel objects (src/kernel/**)
KERN_OBJ = $(patsubst src/kernel/%.c,$(BUILD)/kernel/%.o,$(wildcard src/kernel/**/*.c))
KERN_DRV = $(patsubst src/drivers/%.c,$(BUILD)/drivers/%.o,$(wildcard src/drivers/**/*.c))

# Init binary (embedded)
$(BUILD)/gen/init_bin.h: $(BUILD)/user/init
    python3 tools/embed.py $< init_bin > $@

# Link
$(BUILD)/BOOTX64.EFI: $(KERN_OBJ) $(KERN_DRV) $(BUILD)/gen/init_bin.h
    ...

clean:
    rm -rf build/

# Delegations
test-hw test-crash test-fuzz:
    $(MAKE) -C test $@

alpine-image qemu-alpine:
    $(MAKE) -C alpine $@
```

### test/Makefile

```makefile
# CosmoRT Test Build
KTEST_UNIT = $(wildcard unit/**/*.c)
KTEST_CRASH = $(wildcard crash/*.c)
KTEST_FUZZ = $(wildcard fuzz/*.c)

# Baut eigene init_bin.h (ueberschreibt nie die Root-Version)
TEST_INIT = $(BUILD)/test/init_bin.h

# ktest Binary aus test/main.c + unit tests
$(BUILD)/user/ktest: $(CRT0) $(KTEST_UNIT_OBJ) init.ld
    $(LD) -T init.ld -o $@ $(CRT0) $(KTEST_UNIT_OBJ)

# QEMU run: eigenes ESP Image mit ktest als init
hw: $(BUILD)/user/ktest
    # Embed ktest als init_bin.h (in test-eigenes Verzeichnis)
    # Build Kernel mit TEST init
    # Boot QEMU
    ...

crash: $(BUILD)/user/ktest-crash
    ...

fuzz: $(BUILD)/user/ktest-fuzz
    ...
```

### alpine/Makefile (oder tools/Makefile)

```makefile
# Alpine Image Build
ALPINE_ROOT ?= /tmp/alpine-root

image: build/alpine.img build/disk.img

build/alpine.img: $(ALPINE_ROOT)
    mkfs.ext2 -d $(ALPINE_ROOT) -r 1 -N 4096 $@ 512M

# QEMU mit Alpine + Kernel init.c
run: image
    # Baut Kernel mit init.c (nicht ktest!)
    # Boot QEMU mit alpine.img
    ...
```

### src/arch/x86_64/Makefile.arch

```makefile
# x86_64 spezifisch
KCFLAGS += -mno-red-zone -mno-sse -mgeneral-regs-only
UCFLAGS += -ffreestanding -fno-stack-protector

# Boot
$(BUILD)/boot/boot.o: src/boot/boot.c
    ...

# Trampoline
$(BUILD)/gen/ap_trampoline_bin.h: src/arch/x86_64/ap_trampoline.S
    ...

# Syscall entry
$(BUILD)/arch/syscall_entry.o: src/arch/x86_64/syscall_entry.asm
    ...
```

## Konfiguration

`.config` im Root (nicht getrackt, in .gitignore):
```makefile
ARCH=x86_64
```

Root Makefile liest `.config` mit Fallback:
```makefile
-include .config
ARCH ?= x86_64
```

Einmal setzen: `echo "ARCH=x86_64" > .config`
Cross-Compile: `echo "ARCH=aarch64" > .config` oder temporaer `make ARCH=aarch64`
CLI ueberschreibt .config (Make-Konvention: CLI > File > Default).

.config ist fuer ALLE Makefiles relevant (Root, test/, alpine/).
Jedes Sub-Makefile liest `../.config` oder bekommt ARCH vom Root via export.

## Kommandos

```
make                    Kernel bauen (alle Architekturen)
make ARCH=x86_64        Kernel bauen (eine Architektur)
make -C test            Alle Tests (hw, crash, fuzz)
make -C test hw         Unit Tests in QEMU
make -C test crash      Crash Tests in QEMU
make -C test fuzz       Fuzz Tests in QEMU
make -C alpine run      Alpine mit GUI starten (bash interaktiv)
make -C alpine test     Alpine mit test.sh ohne GUI (headless, Serial-Log)
make -C alpine image    ext2 Image bauen (bash default, Testsuites kompiliert)
```

## Prinzipien

1. `make` baut NUR den Kernel — alle Architekturen (x86_64, spaeter aarch64, riscv64)
2. `make -C test` baut und testet — eigenes ESP, eigenes init (ktest), ueberschreibt nie den Kernel
3. `make -C alpine run` startet Alpine mit GUI (bash), `test` startet headless mit Script
4. Keine Seiteneffekte: kein Target veraendert Output eines anderen Targets
5. Separate Build-Verzeichnisse: build/kernel/, build/test/, build/alpine/

## Alpine Image Inhalt

`make -C alpine image` baut ein ext2 Image aus Alpine minirootfs mit:

### Default Shell: bash
- init.c: `execve("/bin/bash")` statt `/bin/sh`
- bash vorinstalliert (apk add bash)
- /bin/sh → busybox bleibt (fuer Scripts mit #!/bin/sh)

### Vorinstallierte Pakete
```
bash gcc musl-dev make git nodejs npm
stress-ng linux-headers autoconf automake libtool m4 pkgconf
```

### Kompilierte Testsuites

#### LTP (Linux Test Project)
- Source: https://github.com/linux-test-project/ltp
- Installiert: /opt/ltp/
- Build: `cd /opt/ltp && make autotools && ./configure && make -j4`
- Run: `cd /opt/ltp && ./runltp -f syscalls` (oder `-f mm`, `-f ipc`, `-f fs`)
- ~378 kompilierte Syscall-Test-Binaries
- Deckt ab: mmap, clone, futex, pipe, socket, signal, epoll, inotify, ...

#### musl libc-test
- Source: https://repo.or.cz/libc-test.git
- Installiert: /opt/libc-test/
- Build: `cd /opt/libc-test && make`
- Run: `cd /opt/libc-test && make`  (baut und testet in einem Schritt)
- ~479 Test-Binaries (functional, math, regression)
- Deckt ab: POSIX libc Conformance, pthreads, math, string, regex, ...

#### stress-ng
- Installiert via: `apk add stress-ng`
- Run: `stress-ng --cpu 2 --vm 1 --fork 4 --timeout 60s`
- ~300 Stressoren fuer CPU, Memory, I/O, Scheduler, Syscalls
- Findet Crashes und Deadlocks unter Last

### Test-Scripts
```
/tmp/test_gcc.sh       gcc Kompilierung testen
/tmp/test_npm.sh       npm Version + install testen
/tmp/test_node.sh      Node.js hello world
```

### Build-Prozess (auf Host, im chroot)
1. Alpine minirootfs herunterladen + extrahieren
2. apk add: alle Pakete installieren
3. LTP clonen + bauen (make autotools, configure, make)
4. musl libc-test clonen + bauen (make)
5. mkfs.ext2 -d → build/alpine.img

## Migration

Phase 1: Separate ESP Images fuer test und alpine (zwei BOOTX64.EFI)
Phase 2: Makefiles aufteilen
Phase 3: Root Makefile entschlacken
