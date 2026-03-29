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

## Prinzipien

1. `make` in Root baut NUR den Kernel (BOOTX64.EFI mit init.c)
2. `make -C test hw` baut Tests und booted in QEMU — ueberschreibt nie den Kernel-Init
3. `make -C alpine image` baut das ext2 Image — unabhaengig vom Kernel Build
4. Keine Seiteneffekte: kein Target veraendert Output eines anderen Targets
5. init_bin.h existiert zweimal: Root (init.c) und test/ (ktest)

## Migration

Phase 1: Separate ESP Images fuer test und alpine (zwei BOOTX64.EFI)
Phase 2: Makefiles aufteilen
Phase 3: Root Makefile entschlacken
