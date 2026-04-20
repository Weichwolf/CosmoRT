# CosmoRT Test Build
# Builds ktest binaries with SEPARATE init_bin.h and ESP image.
# Never touches build/gen/init_bin.h (kernel's init).
#
# Usage:
#   make -C test         Unit tests (default)
#   make -C test hw      Unit tests in QEMU
#   make -C test crash   Crash tests in QEMU
#   make -C test fuzz    Fuzz tests in QEMU

ROOT     = $(CURDIR)/..
SRC      = $(ROOT)/src
BUILD    = $(ROOT)/build
TESTDIR  = $(ROOT)/test
ARCH_DIR = $(SRC)/arch/x86_64
TBUILD   = $(BUILD)/test

CC       = gcc
LD       = ld
OBJCOPY  = objcopy

# ── Flags (mirror root Makefile) ──
UCFLAGS  = -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-plt -mno-red-zone -nostdlib -O2 \
           -I$(ROOT)/include/public

TCFLAGS  = $(UCFLAGS) -MMD -MP -I$(ROOT)/include/kernel -I$(ROOT)/include -I$(TESTDIR)

KCFLAGS  = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
           -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
           -Wall -Wextra -Werror -O2 -c -MMD -MP \
           -I$(ROOT)/include/public -I$(ROOT)/include/kernel -I$(ROOT)/include \
           -I$(SRC)/kernel -I$(BUILD) -std=c11

EFI_LIB  = /usr/lib
EFI_CRT  = $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS  = $(EFI_LIB)/elf_x86_64_efi.lds

LDFLAGS  = -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
           -Bsymbolic -L$(EFI_LIB)

CRT0     = $(BUILD)/user/crt0.o

# ── Kernel object list (shared with root) ──
include $(ROOT)/kern_objects.mk

# ── Test object files ──
KTEST_UNIT_OBJ = $(TBUILD)/main.o \
    $(patsubst $(TESTDIR)/unit/mm/%.c,$(TBUILD)/unit/mm/%.o,$(wildcard $(TESTDIR)/unit/mm/*.c)) \
    $(patsubst $(TESTDIR)/unit/fs/%.c,$(TBUILD)/unit/fs/%.o,$(wildcard $(TESTDIR)/unit/fs/*.c)) \
    $(patsubst $(TESTDIR)/unit/ipc/%.c,$(TBUILD)/unit/ipc/%.o,$(wildcard $(TESTDIR)/unit/ipc/*.c)) \
    $(patsubst $(TESTDIR)/unit/sched/%.c,$(TBUILD)/unit/sched/%.o,$(wildcard $(TESTDIR)/unit/sched/*.c)) \
    $(patsubst $(TESTDIR)/unit/signal/%.c,$(TBUILD)/unit/signal/%.o,$(wildcard $(TESTDIR)/unit/signal/*.c)) \
    $(patsubst $(TESTDIR)/unit/sys/%.c,$(TBUILD)/unit/sys/%.o,$(wildcard $(TESTDIR)/unit/sys/*.c)) \
    $(patsubst $(TESTDIR)/unit/hw/%.c,$(TBUILD)/unit/hw/%.o,$(wildcard $(TESTDIR)/unit/hw/*.c)) \
    $(patsubst $(TESTDIR)/unit/net/%.c,$(TBUILD)/unit/net/%.o,$(wildcard $(TESTDIR)/unit/net/*.c)) \
    $(patsubst $(TESTDIR)/unit/proc/%.c,$(TBUILD)/unit/proc/%.o,$(wildcard $(TESTDIR)/unit/proc/*.c)) \
    $(patsubst $(TESTDIR)/unit/perf/%.c,$(TBUILD)/unit/perf/%.o,$(wildcard $(TESTDIR)/unit/perf/*.c)) \
    $(patsubst $(TESTDIR)/libc/%.c,$(TBUILD)/libc/%.o,$(wildcard $(TESTDIR)/libc/*.c)) \
    $(patsubst $(TESTDIR)/ltp/%.c,$(TBUILD)/ltp/%.o,$(wildcard $(TESTDIR)/ltp/*.c))

KTEST_CRASH_OBJ = $(TBUILD)/main.o \
    $(patsubst $(TESTDIR)/crash/%.c,$(TBUILD)/crash/%.o,$(wildcard $(TESTDIR)/crash/*.c))

KTEST_FUZZ_OBJ = $(TBUILD)/main.o \
    $(patsubst $(TESTDIR)/fuzz/%.c,$(TBUILD)/fuzz/%.o,$(wildcard $(TESTDIR)/fuzz/*.c))

# ── Phony targets ──
.PHONY: all hw crash fuzz clean kernel-deps

all: hw

# ── Directories ──
$(TBUILD) $(TBUILD)/unit $(TBUILD)/crash $(TBUILD)/fuzz \
$(TBUILD)/unit/mm $(TBUILD)/unit/fs $(TBUILD)/unit/ipc \
$(TBUILD)/unit/sched $(TBUILD)/unit/signal $(TBUILD)/unit/sys \
$(TBUILD)/unit/hw $(TBUILD)/unit/net $(TBUILD)/unit/proc \
$(TBUILD)/unit/perf \
$(TBUILD)/libc \
$(TBUILD)/ltp \
$(TBUILD)/hw/gen $(TBUILD)/crash/gen $(TBUILD)/fuzz/gen:
	@mkdir -p $@

# ── Ensure kernel objects + CRT0 are built ──
kernel-deps:
	@$(MAKE) -C $(ROOT) kernel-objs $(CRT0)

# ── Compile test sources ──
$(TBUILD)/main.o: $(TESTDIR)/main.c $(TESTDIR)/ktest.h | $(TBUILD)
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/mm/%.o: $(TESTDIR)/unit/mm/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/mm
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/fs/%.o: $(TESTDIR)/unit/fs/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/fs
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/ipc/%.o: $(TESTDIR)/unit/ipc/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/ipc
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/sched/%.o: $(TESTDIR)/unit/sched/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/sched
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/signal/%.o: $(TESTDIR)/unit/signal/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/signal
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/sys/%.o: $(TESTDIR)/unit/sys/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/sys
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/hw/%.o: $(TESTDIR)/unit/hw/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/hw
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/net/%.o: $(TESTDIR)/unit/net/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/net
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/proc/%.o: $(TESTDIR)/unit/proc/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/proc
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/unit/perf/%.o: $(TESTDIR)/unit/perf/%.c $(TESTDIR)/ktest.h | $(TBUILD)/unit/perf
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/libc/%.o: $(TESTDIR)/libc/%.c $(TESTDIR)/ktest.h | $(TBUILD)/libc
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/ltp/%.o: $(TESTDIR)/ltp/%.c $(TESTDIR)/ktest.h | $(TBUILD)/ltp
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/crash/%.o: $(TESTDIR)/crash/%.c $(TESTDIR)/ktest.h | $(TBUILD)/crash
	$(CC) $(TCFLAGS) -c -o $@ $<

$(TBUILD)/fuzz/%.o: $(TESTDIR)/fuzz/%.c $(TESTDIR)/ktest.h | $(TBUILD)/fuzz
	$(CC) $(TCFLAGS) $(if $(FUZZ_SEED),-DFUZZ_SEED=$(FUZZ_SEED)) -c -o $@ $<

# ── Link test binaries ──
$(TBUILD)/ktest: $(CRT0) $(KTEST_UNIT_OBJ) $(SRC)/user/init.ld | kernel-deps
	$(LD) -T $(SRC)/user/init.ld -o $@ $(CRT0) $(KTEST_UNIT_OBJ)

$(TBUILD)/ktest-crash: $(CRT0) $(KTEST_CRASH_OBJ) $(SRC)/user/init.ld | kernel-deps
	$(LD) -T $(SRC)/user/init.ld -o $@ $(CRT0) $(KTEST_CRASH_OBJ)

$(TBUILD)/ktest-fuzz: $(CRT0) $(KTEST_FUZZ_OBJ) $(SRC)/user/init.ld | kernel-deps
	$(LD) -T $(SRC)/user/init.ld -o $@ $(CRT0) $(KTEST_FUZZ_OBJ)

# ── Embed test binary as gen/init_bin.h (per-variant subdir) ──
# Each variant gets its own gen/init_bin.h so main.c's #include "gen/init_bin.h" resolves correctly
$(TBUILD)/hw/gen/init_bin.h: $(TBUILD)/ktest | $(TBUILD)/hw/gen
	@python3 $(ROOT)/tools/embed.py $< init_bin > $@
	@echo "test/hw init_bin.h: $$(wc -c < $<) bytes"

$(TBUILD)/crash/gen/init_bin.h: $(TBUILD)/ktest-crash | $(TBUILD)/crash/gen
	@python3 $(ROOT)/tools/embed.py $< init_bin > $@
	@echo "test/crash init_bin.h: $$(wc -c < $<) bytes"

$(TBUILD)/fuzz/gen/init_bin.h: $(TBUILD)/ktest-fuzz | $(TBUILD)/fuzz/gen
	@python3 $(ROOT)/tools/embed.py $< init_bin > $@
	@echo "test/fuzz init_bin.h: $$(wc -c < $<) bytes"

# ── Build kernel main.o with TEST init_bin.h ──
# -I$(TBUILD)/<variant> BEFORE -I$(BUILD) so test's gen/init_bin.h wins over kernel's
# KCFLAGS without -I$(BUILD) — we prepend our own variant dir instead
KCFLAGS_NO_BUILD = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
                   -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
                   -Wall -Wextra -Werror -O2 -c -MMD -MP \
                   -I$(ROOT)/include/public -I$(ROOT)/include/kernel -I$(ROOT)/include \
                   -I$(SRC)/kernel -std=c11

$(TBUILD)/hw/main_kern.o: $(SRC)/kernel/core/main.c $(TBUILD)/hw/gen/init_bin.h | kernel-deps
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS_NO_BUILD) -I$(TBUILD)/hw -I$(BUILD) -I$(SRC)/kernel/gen -o $@ $<

$(TBUILD)/crash/main_kern.o: $(SRC)/kernel/core/main.c $(TBUILD)/crash/gen/init_bin.h | kernel-deps
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS_NO_BUILD) -I$(TBUILD)/crash -I$(BUILD) -I$(SRC)/kernel/gen -o $@ $<

$(TBUILD)/fuzz/main_kern.o: $(SRC)/kernel/core/main.c $(TBUILD)/fuzz/gen/init_bin.h | kernel-deps
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS_NO_BUILD) -I$(TBUILD)/fuzz -I$(BUILD) -I$(SRC)/kernel/gen -o $@ $<

# ── Link test kernel (swap main.o, reuse all other kernel .o) ──
$(TBUILD)/hw/cosmo-rt.so: $(TBUILD)/hw/main_kern.o $(KERN_OBJ_NO_MAIN) $(BUILD)/boot/boot.o
	$(LD) $(LDFLAGS) $(EFI_CRT) $(BUILD)/boot/boot.o $< $(KERN_OBJ_NO_MAIN) -o $@ -lefi -lgnuefi

$(TBUILD)/crash/cosmo-rt.so: $(TBUILD)/crash/main_kern.o $(KERN_OBJ_NO_MAIN) $(BUILD)/boot/boot.o
	$(LD) $(LDFLAGS) $(EFI_CRT) $(BUILD)/boot/boot.o $< $(KERN_OBJ_NO_MAIN) -o $@ -lefi -lgnuefi

$(TBUILD)/fuzz/cosmo-rt.so: $(TBUILD)/fuzz/main_kern.o $(KERN_OBJ_NO_MAIN) $(BUILD)/boot/boot.o
	$(LD) $(LDFLAGS) $(EFI_CRT) $(BUILD)/boot/boot.o $< $(KERN_OBJ_NO_MAIN) -o $@ -lefi -lgnuefi

# ── EFI binaries ──
$(TBUILD)/hw/BOOTX64.EFI: $(TBUILD)/hw/cosmo-rt.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	           -j .dynsym -j .rel -j .rela -j .reloc -j .bss \
	           --target=efi-app-x86_64 $< $@

$(TBUILD)/crash/BOOTX64.EFI: $(TBUILD)/crash/cosmo-rt.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	           -j .dynsym -j .rel -j .rela -j .reloc -j .bss \
	           --target=efi-app-x86_64 $< $@

$(TBUILD)/fuzz/BOOTX64.EFI: $(TBUILD)/fuzz/cosmo-rt.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	           -j .dynsym -j .rel -j .rela -j .reloc -j .bss \
	           --target=efi-app-x86_64 $< $@

# ── ESP images ──
define make_esp
	dd if=/dev/zero of=$(1) bs=1M count=64 2>/dev/null
	mformat -i $(1) -F ::
	mmd -i $(1) ::/EFI
	mmd -i $(1) ::/EFI/BOOT
	mcopy -i $(1) $(2) ::/EFI/BOOT/BOOTX64.EFI
endef

$(TBUILD)/hw/cosmo-rt.img: $(TBUILD)/hw/BOOTX64.EFI
	$(call make_esp,$@,$<)

$(TBUILD)/crash/cosmo-rt.img: $(TBUILD)/crash/BOOTX64.EFI
	$(call make_esp,$@,$<)

$(TBUILD)/fuzz/cosmo-rt.img: $(TBUILD)/fuzz/BOOTX64.EFI
	$(call make_esp,$@,$<)

# ── QEMU ──
QEMU = qemu-system-x86_64

define run_qemu
	@rm -f /tmp/cosmo-serial.log
	timeout 180 $(QEMU) -cpu qemu64,+smep,+smap -smp 2 -m 4096 \
	  -bios /usr/share/ovmf/OVMF.fd \
	  -drive file=$(1),format=raw \
	  -serial file:/tmp/cosmo-serial.log \
	  -display none -no-reboot \
	  -device e1000,netdev=net0 \
	  -netdev user,id=net0 || true
	@echo "=== Test Results ==="
	@grep -E "=== |FAIL" /tmp/cosmo-serial.log || true
endef

# ── Main targets ──
hw: $(TBUILD)/hw/cosmo-rt.img
	$(call run_qemu,$(TBUILD)/hw/cosmo-rt.img)

crash: $(TBUILD)/crash/cosmo-rt.img
	$(call run_qemu,$(TBUILD)/crash/cosmo-rt.img)

fuzz: $(TBUILD)/fuzz/cosmo-rt.img
	$(call run_qemu,$(TBUILD)/fuzz/cosmo-rt.img)

clean:
	rm -rf $(TBUILD)

# ── Header dependency tracking ──
TEST_OBJ = $(KTEST_UNIT_OBJ) $(KTEST_CRASH_OBJ) $(KTEST_FUZZ_OBJ) \
           $(TBUILD)/hw/main_kern.o $(TBUILD)/crash/main_kern.o $(TBUILD)/fuzz/main_kern.o
-include $(TEST_OBJ:.o=.d)
