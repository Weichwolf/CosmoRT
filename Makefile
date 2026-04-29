# CosmoRT Build System

HOST_CC  = gcc
CC       = gcc
NASM     = nasm
LD       = ld
OBJCOPY  = objcopy

SRC      = src
BUILD    = build
ARCH_DIR = src/arch/x86_64

EFI_INC  = /usr/include/efi
EFI_LIB  = /usr/lib
EFI_CRT  = $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS  = $(SRC)/boot/efi_x86_64.lds

EFI_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
             -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
             -Wall -Wextra -Werror -O2 -c -MMD -MP \
             -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_INC)/protocol \
             -DGNU_EFI_USE_MS_ABI

KCFLAGS  = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
           -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
           -Wall -Wextra -Werror -O2 -c -MMD -MP \
           -Iinclude/public -Iinclude/kernel -Iinclude -I$(SRC)/kernel -I$(BUILD) -std=c11

# Drivers: only public headers (cosmort.h) + own subdirectory
DRVFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
           -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
           -Wall -Wextra -Werror -O2 -c -MMD -MP \
           -Iinclude/public -std=c11

LDFLAGS  = -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
           -Bsymbolic -L$(EFI_LIB) -z defs

EFI_BIN  = $(BUILD)/BOOTX64.EFI
ESP_IMG  = $(BUILD)/cosmo-rt.img

# ── Object files ──
BOOT_OBJ = $(BUILD)/boot/boot.o

include kern_objects.mk

ALL_OBJ  = $(BOOT_OBJ) $(KERN_OBJ)

.PHONY: all clean qemu stop init-bin disk vhdx kernel-objs \
        test test-hw test-crash test-fuzz test-all alpine-image

all: $(ESP_IMG)

# ── Kernel objects (for test/Makefile) ──
kernel-objs: $(KERN_OBJ_NO_MAIN) $(BOOT_OBJ)

# ── Directories ──────────────────────────────────
KDIRS = $(BUILD)/kernel $(BUILD)/kernel/core $(BUILD)/kernel/mm \
        $(BUILD)/kernel/proc $(BUILD)/kernel/ipc \
        $(BUILD)/kernel/fs $(BUILD)/kernel/net $(BUILD)/kernel/event \
        $(BUILD)/kernel/vt $(BUILD)/kernel/hw $(BUILD)/kernel/sys
DDIRS = $(BUILD)/drivers/virtio $(BUILD)/drivers/pci $(BUILD)/drivers/hyperv

ADIRS = $(BUILD)/arch/x86_64 $(BUILD)/arch/x86_64/boot $(BUILD)/arch/x86_64/cpu \
        $(BUILD)/arch/x86_64/irq $(BUILD)/arch/x86_64/syscall \
        $(BUILD)/arch/x86_64/timer $(BUILD)/arch/x86_64/hw

$(BUILD)/boot $(KDIRS) $(BUILD)/user $(DDIRS) $(ADIRS):
	mkdir -p $@

$(BUILD)/gen:
	@mkdir -p $@

$(BUILD)/gen/font_atlas.h: fonts/font_atlas.h | $(BUILD)/gen
	@cp $< $@

# ── kexec trampoline (64-bit, flat binary → C header) ──
$(BUILD)/kernel/kexec_tramp.bin: $(ARCH_DIR)/hw/kexec_tramp.S | $(BUILD)/kernel
	$(CC) -c -nostdlib -ffreestanding -mno-red-zone -o $(BUILD)/kernel/kexec_tramp_tmp.o $<
	$(OBJCOPY) -O binary -j .text $(BUILD)/kernel/kexec_tramp_tmp.o $@

$(BUILD)/gen/kexec_tramp_bin.h: $(BUILD)/kernel/kexec_tramp.bin | $(BUILD)/gen
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated kexec trampoline (%d bytes) */' % len(data)); \
	print('static const unsigned char kexec_tramp_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long kexec_tramp_bin_size = %d;' % len(data))" > $@
	@echo "kexec_tramp_bin.h: $$(wc -c < $<) bytes"

# kexec.o depends on trampoline header
$(BUILD)/kernel/hw/kexec.o: $(SRC)/kernel/hw/kexec.c $(BUILD)/gen/kexec_tramp_bin.h | $(BUILD)/kernel/hw
	$(CC) $(KCFLAGS) -I$(SRC)/kernel/gen -o $@ $<

# ── CRT0: ABI-correct _start → _start_c trampoline ──
$(BUILD)/user/crt0.o: $(SRC)/user/crt0.S | $(BUILD)/user
	$(CC) -c -MMD -MP -o $@ $<

CRT0 = $(BUILD)/user/crt0.o

# ── Init binary (embedded in kernel) ─────────────
$(BUILD)/user/init.o: $(SRC)/user/init.c | $(BUILD)/user
	$(CC) -ffreestanding -fno-stack-protector -fno-stack-check \
	      -fno-plt -mno-red-zone -nostdlib -O2 -c -MMD -MP -o $@ $<

$(BUILD)/user/init: $(CRT0) $(BUILD)/user/init.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $(CRT0) $(BUILD)/user/init.o

$(BUILD)/gen/init_bin.h: $(BUILD)/user/init | $(BUILD)/gen
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated init binary (%d bytes) */' % len(data)); \
	print('static const unsigned char init_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long init_bin_size = %d;' % len(data))" > $@
	@echo "init_bin.h: $$(wc -c < $<) bytes"

init-bin: $(BUILD)/gen/init_bin.h

# ── vDSO (embedded user-space DSO with __vdso_clock_gettime) ─────
# Built as a shared object linked at fixed VA VDSO_CODE_VA. Compiled
# with -fpic but loaded at a static address, so PC-relative addressing
# of the data page (one page below) needs no runtime relocation.
VDSO_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
              -fno-plt -mno-red-zone -mno-sse -mno-mmx -mno-sse2 \
              -mgeneral-regs-only -nostdlib -O2 -fpic -fvisibility=hidden \
              -Wall -Wextra -Werror -std=c11 -c -MMD -MP

$(BUILD)/user/vdso.o: $(SRC)/user/vdso/vdso.c | $(BUILD)/user
	$(CC) $(VDSO_CFLAGS) -o $@ $<

$(BUILD)/user/vdso.so: $(BUILD)/user/vdso.o $(SRC)/user/vdso/vdso.lds $(SRC)/user/vdso/vdso.ver
	$(LD) -nostdlib -shared -Bsymbolic --no-undefined -z now -z noexecstack \
	      --hash-style=both --build-id=none \
	      --version-script=$(SRC)/user/vdso/vdso.ver \
	      -T $(SRC)/user/vdso/vdso.lds -soname=linux-vdso.so.1 \
	      -o $@ $(BUILD)/user/vdso.o
	@strip --strip-debug $@ 2>/dev/null || true

$(BUILD)/gen/vdso_bin.h: $(BUILD)/user/vdso.so | $(BUILD)/gen
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated vDSO binary (%d bytes) */' % len(data)); \
	print('static const unsigned char vdso_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long vdso_bin_size = %d;' % len(data))" > $@
	@echo "vdso_bin.h: $$(wc -c < $<) bytes"

vdso-bin: $(BUILD)/gen/vdso_bin.h

# ── Alpine disk image (ext2) ─────────────────────
ALPINE_ROOT ?= build/alpine-root

alpine-image: $(EFI_BIN)
	sh tools/mkalpine.sh $(ALPINE_ROOT)

$(BUILD)/disk.img: $(EFI_BIN) | $(BUILD)
	sh tools/mkalpine.sh

disk: $(BUILD)/disk.img

# ── Benchmark binary ────────────────────────────
UCFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-plt -mno-red-zone -nostdlib -O2 \
          -Iinclude/public

# ── Bootloader (EFI) ────────────────────────────
$(BUILD)/boot/boot.o: $(SRC)/boot/boot.c | $(BUILD)/boot
	$(CC) $(EFI_CFLAGS) -o $@ $<

# ── Architecture ASM (src/arch/x86_64/{boot,cpu,irq,syscall}/) ──
ASFLAGS = -c -MMD -MP -ffreestanding -mno-red-zone -nostdlib

$(BUILD)/kernel/entry.o: $(ARCH_DIR)/boot/entry.S | $(BUILD)/kernel
	$(CC) $(ASFLAGS) -o $@ $<

$(BUILD)/kernel/irq_asm.o: $(ARCH_DIR)/irq/irq_asm.S | $(BUILD)/kernel
	$(CC) $(ASFLAGS) -o $@ $<

$(BUILD)/kernel/context.o: $(ARCH_DIR)/cpu/context.S | $(BUILD)/kernel
	$(CC) $(ASFLAGS) -o $@ $<

$(BUILD)/kernel/syscall_entry.o: $(ARCH_DIR)/syscall/syscall_entry.S | $(BUILD)/kernel
	$(CC) $(ASFLAGS) -o $@ $<

# ── Architecture C (src/arch/x86_64/) ─────────────

# SHA-256: compiled without -mno-sse (RT-Core has no user FPU state to protect)
SHA256_CFLAGS = $(subst -mno-sse,,$(subst -mno-sse2,,$(subst -mno-mmx,,$(subst -mgeneral-regs-only,,$(KCFLAGS)))))
$(BUILD)/arch/x86_64/sha256.o: $(ARCH_DIR)/hw/sha256.c | $(ADIRS)
	$(CC) $(SHA256_CFLAGS) -o $@ $<

$(BUILD)/arch/x86_64/%.o: $(ARCH_DIR)/%.c | $(ADIRS)
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/memops.o: $(SRC)/kernel/mm/memops.c | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -o $@ $<

# ── Kernel C (subdirectories) ────────────────────
$(BUILD)/kernel/core/%.o: $(SRC)/kernel/core/%.c | $(BUILD)/kernel/core
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/mm/%.o: $(SRC)/kernel/mm/%.c | $(BUILD)/kernel/mm
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/proc/%.o: $(SRC)/kernel/proc/%.c | $(BUILD)/kernel/proc
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/sys/%.o: $(SRC)/kernel/sys/%.c | $(BUILD)/kernel/sys
	$(CC) $(KCFLAGS) -I$(SRC)/kernel/sys -o $@ $<

# vdso.o needs gen/vdso_bin.h (the embedded user-space DSO blob)
$(BUILD)/kernel/sys/vdso.o: $(SRC)/kernel/sys/vdso.c $(BUILD)/gen/vdso_bin.h | $(BUILD)/kernel/sys
	$(CC) $(KCFLAGS) -I$(SRC)/kernel/sys -I$(SRC)/kernel/gen -o $@ $<

$(BUILD)/kernel/ipc/%.o: $(SRC)/kernel/ipc/%.c | $(BUILD)/kernel/ipc
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/fs/%.o: $(SRC)/kernel/fs/%.c | $(BUILD)/kernel/fs
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/net/%.o: $(SRC)/kernel/net/%.c | $(BUILD)/kernel/net
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/event/%.o: $(SRC)/kernel/event/%.c | $(BUILD)/kernel/event
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/vt/%.o: $(SRC)/kernel/vt/%.c $(BUILD)/gen/font_atlas.h | $(BUILD)/kernel/vt
	$(CC) $(KCFLAGS) -o $@ $<

$(BUILD)/kernel/hw/%.o: $(SRC)/kernel/hw/%.c | $(BUILD)/kernel/hw
	$(CC) $(KCFLAGS) -o $@ $<

# ── Drivers (public headers only + own subdirectory) ──
$(BUILD)/drivers/virtio/%.o: $(SRC)/drivers/virtio/%.c | $(BUILD)/drivers/virtio
	$(CC) $(DRVFLAGS) -I$(SRC)/drivers/virtio -o $@ $<

$(BUILD)/drivers/pci/%.o: $(SRC)/drivers/pci/%.c | $(BUILD)/drivers/pci
	$(CC) $(DRVFLAGS) -I$(SRC)/drivers/pci -o $@ $<

# Hyper-V drivers: need hyperv.h from kernel/ (kernel-side MSR/SynIC defs)
$(BUILD)/drivers/hyperv/%.o: $(SRC)/drivers/hyperv/%.c | $(BUILD)/drivers/hyperv
	$(CC) $(DRVFLAGS) -I$(SRC)/drivers/hyperv -Iinclude/kernel -Iinclude -o $@ $<

# main.o depends on init_bin.h
$(BUILD)/kernel/core/main.o: $(SRC)/kernel/core/main.c $(BUILD)/gen/init_bin.h | $(BUILD)/kernel/core
	$(CC) $(KCFLAGS) -I$(SRC)/kernel/gen -o $@ $<

# ── Link ────────────────────────────────────────
$(BUILD)/cosmo-rt.so: $(ALL_OBJ) | $(BUILD)
	$(LD) $(LDFLAGS) $(EFI_CRT) $(ALL_OBJ) -o $@ -lefi -lgnuefi

$(EFI_BIN): $(BUILD)/cosmo-rt.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
	           -j .dynsym -j .rel -j .rela -j .reloc -j .bss \
	           --target=efi-app-x86_64 $< $@

$(ESP_IMG): $(EFI_BIN)
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ $(EFI_BIN) ::/EFI/BOOT/BOOTX64.EFI

$(BUILD):
	mkdir -p $(BUILD)

# ── QEMU ────────────────────────────────────────
QEMU = qemu-system-x86_64
QEMU_ACCEL = -machine accel=kvm:tcg
QEMU_FLAGS = $(QEMU_ACCEL) -cpu qemu64,+smep,+smap -smp 2 -m 4096 \
             -bios /usr/share/ovmf/OVMF.fd \
             -drive file=$(ESP_IMG),format=raw \
             -serial stdio \
             -display none \
             -no-reboot \
             -device e1000,netdev=net0 \
             -netdev user,id=net0 \
             -device virtio-net-pci,netdev=net1 \
             -netdev user,id=net1,net=10.0.3.0/24

qemu: $(ESP_IMG)
	$(QEMU) $(QEMU_FLAGS)

# Alpine QEMU flags (shared)
ALPINE_QEMU = $(QEMU) $(QEMU_ACCEL) -cpu qemu64,+smep,+smap -smp 2 -m 4096 \
  -bios /usr/share/ovmf/OVMF.fd \
  -drive file=$(ESP_IMG),format=raw \
  -drive file=build/alpine.img,format=raw,if=virtio \
  -device e1000,netdev=net0 \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net1 \
  -netdev user,id=net1,net=10.0.3.0/24

# make qemu-alpine — normal boot (OpenRC + getty + login)
qemu-alpine: alpine-image
	$(ALPINE_QEMU) -serial stdio -display none -no-reboot

# make qemu-alpine-gui — GUI with keyboard
qemu-alpine-gui: alpine-image
	$(ALPINE_QEMU) -serial file:/tmp/cosmo-serial.log -display gtk -device virtio-keyboard-pci

# make alpine-test — headless boot test (musl + LTP, fail-fast + poweroff)
# Temporarily replaces /etc/inittab, builds image, restores after
alpine-test: $(ESP_IMG)
	@cp $(ALPINE_ROOT)/etc/inittab $(ALPINE_ROOT)/etc/inittab.bak 2>/dev/null || true
	@echo '::sysinit:/opt/boot-test.sh' > $(ALPINE_ROOT)/etc/inittab
	@sh tools/mkalpine.sh $(ALPINE_ROOT)
	@cp $(ALPINE_ROOT)/etc/inittab.bak $(ALPINE_ROOT)/etc/inittab 2>/dev/null || true
	@rm -f /tmp/cosmo-serial.log
	timeout 3600 $(ALPINE_QEMU) -serial file:/tmp/cosmo-serial.log -display none -no-reboot || true
	@echo "=== Serial output ==="
	@tail -30 /tmp/cosmo-serial.log

# make qemu-bench — gcc-compile benchmark inside CosmoRT VM
# Runs tools/cosmo-bench.sh as sysinit; reports per-phase elapsed_ms.
qemu-bench: $(ESP_IMG)
	@cp $(ALPINE_ROOT)/etc/inittab $(ALPINE_ROOT)/etc/inittab.bak 2>/dev/null || true
	@cp tools/cosmo-bench.sh $(ALPINE_ROOT)/opt/cosmo-bench.sh
	@chmod +x $(ALPINE_ROOT)/opt/cosmo-bench.sh
	@echo '::sysinit:/opt/cosmo-bench.sh' > $(ALPINE_ROOT)/etc/inittab
	@sh tools/mkalpine.sh $(ALPINE_ROOT)
	@cp $(ALPINE_ROOT)/etc/inittab.bak $(ALPINE_ROOT)/etc/inittab 2>/dev/null || true
	@rm -f /tmp/cosmo-bench.log
	timeout 600 $(ALPINE_QEMU) -serial file:/tmp/cosmo-bench.log -display none -no-reboot || true
	@echo "=== Bench Output ==="
	@grep -E 'BENCH |COSMO-BENCH|TLB flush|fast=|Page cache|hit=' /tmp/cosmo-bench.log | head -40
	@echo ""

qemu-disk: $(BUILD)/disk.img
	$(QEMU) $(QEMU_FLAGS) \
	        -drive file=build/root.ext2,format=raw,if=virtio

# Interactive bash on VT with ext2 disk
qemu-shell: $(ESP_IMG)
	COSMO_INTERACTIVE=1 sh tools/mkalpine.sh
	$(QEMU) $(subst -display none,-display gtk,$(subst -no-reboot,,$(QEMU_FLAGS))) \
	        -drive file=build/root.ext2,format=raw,if=virtio \
	        -device virtio-keyboard-pci

vhdx: $(BUILD)/disk.img
	qemu-img convert -f raw -O vhdx $(BUILD)/disk.img $(BUILD)/disk.vhdx
	@echo "disk.vhdx: $$(du -h $(BUILD)/disk.vhdx | cut -f1)"

qemu-net: $(ESP_IMG)
	$(QEMU) $(QEMU_FLAGS)

# Background QEMU with serial log
run: $(ESP_IMG)
	@rm -f /tmp/cosmo-serial.log
	$(QEMU) $(QEMU_ACCEL) -cpu qemu64,+smep,+smap -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot &

stop:
	@pkill -f "qemu-system.*cosmo-rt" 2>/dev/null; true

# ── Test (delegated to test/Makefile) ───────────
test test-hw:
	$(MAKE) -C test hw

test-crash:
	$(MAKE) -C test crash

test-fuzz:
	$(MAKE) -C test fuzz

test-all:
	$(MAKE) -C test hw
	$(MAKE) -C test crash
	$(MAKE) -C test fuzz

# ── Boot test ───────────────────────────────────
test-boot: $(ESP_IMG)
	@rm -f /tmp/cosmo-serial.log
	timeout 10 $(QEMU) $(QEMU_ACCEL) -cpu qemu64,+smep,+smap -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot \
	        -device e1000,netdev=net0 \
	        -netdev user,id=net0 \
	        -device virtio-net-pci,netdev=net1 \
	        -netdev user,id=net1,net=10.0.3.0/24 || true
	@echo "=== Serial output ==="
	@cat /tmp/cosmo-serial.log 2>/dev/null || echo "(no output)"
	@grep -q "CosmoOS booted" /tmp/cosmo-serial.log 2>/dev/null && \
	  echo "=== PASS ===" || echo "=== FAIL ==="

test-boot-disk: $(BUILD)/disk.img
	@rm -f /tmp/cosmo-serial.log
	timeout 10 $(QEMU) $(QEMU_ACCEL) -cpu qemu64,+smep,+smap -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(BUILD)/disk.img,format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot \
	        -device e1000,netdev=net0 \
	        -netdev user,id=net0 \
	        -device virtio-net-pci,netdev=net1 \
	        -netdev user,id=net1,net=10.0.3.0/24 || true
	@echo "=== Serial output ==="
	@cat /tmp/cosmo-serial.log 2>/dev/null || echo "(no output)"
	@grep -q "CosmoOS booted" /tmp/cosmo-serial.log 2>/dev/null && \
	  echo "=== PASS ===" || echo "=== FAIL ==="

clean:
	find $(BUILD) -mindepth 1 -maxdepth 1 ! -name alpine-root -exec rm -rf {} + 2>/dev/null || true

# ── Header dependency tracking ──
# -MMD -MP in CFLAGS/ASFLAGS emits .d files alongside .o. Include them so
# a header change triggers rebuild of every .o that #includes it. Prefix `-`
# ignores missing .d on first build.
DEPS = $(ALL_OBJ:.o=.d) $(BUILD)/user/init.d $(BUILD)/user/crt0.d
-include $(DEPS)
