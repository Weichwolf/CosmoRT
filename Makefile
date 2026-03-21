# CosmoRT Build System

HOST_CC  = gcc
CC       = gcc
NASM     = nasm
LD       = ld
OBJCOPY  = objcopy

SRC      = src
BUILD    = build

EFI_INC  = /usr/include/efi
EFI_LIB  = /usr/lib
EFI_CRT  = $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS  = $(EFI_LIB)/elf_x86_64_efi.lds

EFI_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
             -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
             -Wall -Wextra -Werror -O2 -c \
             -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_INC)/protocol \
             -DGNU_EFI_USE_MS_ABI

KCFLAGS  = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
           -mno-red-zone -Wall -Wextra -Werror -O2 -c \
           -I$(SRC)/kernel -std=c11

LDFLAGS  = -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
           -Bsymbolic -L$(EFI_LIB)

EFI_BIN  = $(BUILD)/BOOTX64.EFI
ESP_IMG  = $(BUILD)/cosmo-rt.img

# Object files
BOOT_OBJ = $(BUILD)/boot/boot.o

KERN_OBJ = $(BUILD)/kernel/entry.o \
           $(BUILD)/kernel/irq_asm.o \
           $(BUILD)/kernel/context.o \
           $(BUILD)/kernel/syscall_entry.o \
           $(BUILD)/kernel/main.o \
           $(BUILD)/kernel/serial.o \
           $(BUILD)/kernel/page_alloc.o \
           $(BUILD)/kernel/paging.o \
           $(BUILD)/kernel/irq.o \
           $(BUILD)/kernel/timer.o \
           $(BUILD)/kernel/tss.o \
           $(BUILD)/kernel/process.o \
           $(BUILD)/kernel/sched.o \
           $(BUILD)/kernel/ipc.o \
           $(BUILD)/kernel/smp.o \
           $(BUILD)/kernel/elf.o \
           $(BUILD)/kernel/syscall.o \
           $(BUILD)/kernel/futex.o \
           $(BUILD)/kernel/slab.o \
           $(BUILD)/kernel/percpu.o \
           $(BUILD)/kernel/vma.o \
           $(BUILD)/kernel/memops.o \
           $(BUILD)/kernel/random.o \
           $(BUILD)/kernel/vfs.o \
           $(BUILD)/kernel/procfs.o \
           $(BUILD)/kernel/hw.o \
           $(BUILD)/kernel/net.o \
           $(BUILD)/kernel/net_port.o \
           $(BUILD)/drivers/virtio/virtio.o \
           $(BUILD)/drivers/net/e1000.o \
           $(BUILD)/drivers/net/virtio_net.o \
           $(BUILD)/drivers/blk/virtio_blk.o \
           $(BUILD)/drivers/gpu/virtio_gpu.o \
           $(BUILD)/drivers/input/virtio_input.o \
           $(BUILD)/kernel/bcache.o \
           $(BUILD)/kernel/btree.o \
           $(BUILD)/kernel/journal.o \
           $(BUILD)/kernel/cosmofs.o \
           $(BUILD)/kernel/kexec.o \
           $(BUILD)/kernel/socket.o \
           $(BUILD)/kernel/epoll.o \
           $(BUILD)/kernel/hyperv.o \
           $(BUILD)/drivers/hyperv/vmbus.o \
           $(BUILD)/drivers/hyperv/storvsc.o \
           $(BUILD)/drivers/hyperv/netvsc.o \
           $(BUILD)/drivers/hyperv/hyperv_fb.o \
           $(BUILD)/drivers/hyperv/hv_kbd.o \
           $(BUILD)/drivers/hyperv/hv_mouse.o \
           $(BUILD)/drivers/hyperv/hv_utils.o \
           $(BUILD)/kernel/pty.o \
           $(BUILD)/kernel/vt.o \
           $(BUILD)/kernel/fb.o \
           $(BUILD)/kernel/input.o

ALL_OBJ  = $(BOOT_OBJ) $(KERN_OBJ)

.PHONY: all clean qemu stop init-bin disk

all: $(ESP_IMG)

# ── Directories ──────────────────────────────────
$(BUILD)/boot $(BUILD)/kernel $(BUILD)/user $(BUILD)/drivers/net $(BUILD)/drivers/blk $(BUILD)/drivers/virtio $(BUILD)/drivers/gpu $(BUILD)/drivers/input $(BUILD)/drivers/hyperv:
	mkdir -p $@

# ── AP trampoline (16-bit, flat binary → C header) ──
$(BUILD)/kernel/ap_trampoline.bin: $(SRC)/kernel/ap_trampoline.asm | $(BUILD)/kernel
	$(NASM) -f bin -o $@ $<

$(SRC)/kernel/ap_trampoline_bin.h: $(BUILD)/kernel/ap_trampoline.bin
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated AP trampoline (%d bytes) */' % len(data)); \
	print('static const unsigned char ap_trampoline_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long ap_trampoline_bin_size = %d;' % len(data))" > $@
	@echo "ap_trampoline_bin.h: $$(wc -c < $<) bytes"

# smp.o depends on trampoline header
$(BUILD)/kernel/smp.o: $(SRC)/kernel/smp.c $(SRC)/kernel/ap_trampoline_bin.h | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -o $@ $<

# ── kexec trampoline (64-bit, flat binary → C header) ──
$(BUILD)/kernel/kexec_tramp.bin: $(SRC)/kernel/kexec_tramp.asm | $(BUILD)/kernel
	$(NASM) -f bin -o $@ $<

$(SRC)/kernel/kexec_tramp_bin.h: $(BUILD)/kernel/kexec_tramp.bin
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
$(BUILD)/kernel/kexec.o: $(SRC)/kernel/kexec.c $(SRC)/kernel/kexec_tramp_bin.h | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -o $@ $<

# ── Init binary (embedded in kernel) ─────────────
$(BUILD)/user/init.o: $(SRC)/user/init.c | $(BUILD)/user
	$(CC) -ffreestanding -fno-stack-protector -fno-stack-check \
	      -fno-plt -mno-red-zone -nostdlib -O2 -c -o $@ $<

$(BUILD)/user/init: $(BUILD)/user/init.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

$(SRC)/kernel/init_bin.h: $(BUILD)/user/init
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated init binary (%d bytes) */' % len(data)); \
	print('static const unsigned char init_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long init_bin_size = %d;' % len(data))" > $@
	@echo "init_bin.h: $$(wc -c < $<) bytes"

init-bin: $(SRC)/kernel/init_bin.h $(SRC)/kernel/ld_cosmo_bin.h $(SRC)/kernel/e1000d_bin.h $(SRC)/kernel/svcmgr_bin.h

# ── Dynamic linker (ld-cosmo.so, embedded in kernel) ──
$(BUILD)/user/ld-cosmo.o: $(SRC)/user/ld-cosmo.c | $(BUILD)/user
	$(CC) -ffreestanding -fno-stack-protector -fno-stack-check \
	      -fno-plt -mno-red-zone -nostdlib -Wall -Wextra -Werror -O2 -c -o $@ $<

$(BUILD)/user/ld-cosmo: $(BUILD)/user/ld-cosmo.o $(SRC)/user/interp.ld
	$(LD) -T $(SRC)/user/interp.ld -o $@ $<

$(SRC)/kernel/ld_cosmo_bin.h: $(BUILD)/user/ld-cosmo
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated ld-cosmo binary (%d bytes) */' % len(data)); \
	print('static const unsigned char ld_cosmo_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long ld_cosmo_bin_size = %d;' % len(data))" > $@
	@echo "ld_cosmo_bin.h: $$(wc -c < $<) bytes"

# ── E1000 userspace driver (embedded in kernel) ──
$(BUILD)/user/e1000d.o: $(SRC)/user/e1000d.c | $(BUILD)/user
	$(CC) $(UCFLAGS) -c -o $@ $<

$(BUILD)/user/e1000d: $(BUILD)/user/e1000d.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

$(SRC)/kernel/e1000d_bin.h: $(BUILD)/user/e1000d
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated e1000d binary (%d bytes) */' % len(data)); \
	print('static const unsigned char e1000d_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long e1000d_bin_size = %d;' % len(data))" > $@
	@echo "e1000d_bin.h: $$(wc -c < $<) bytes"

# ── Service manager (embedded in kernel) ─────────
$(BUILD)/user/svcmgr.o: $(SRC)/user/svcmgr.c | $(BUILD)/user
	$(CC) $(UCFLAGS) -c -o $@ $<

$(BUILD)/user/svcmgr: $(BUILD)/user/svcmgr.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

$(SRC)/kernel/svcmgr_bin.h: $(BUILD)/user/svcmgr
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated svcmgr binary (%d bytes) */' % len(data)); \
	print('static const unsigned char svcmgr_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long svcmgr_bin_size = %d;' % len(data))" > $@
	@echo "svcmgr_bin.h: $$(wc -c < $<) bytes"

# ── mkfs.cosmo (host tool) + disk image ─────────
tools/mkfs: tools/mkfs.c
	$(HOST_CC) -Wall -Wextra -O2 -o $@ $<

disk.img: tools/mkfs
	./tools/mkfs $@ 64

disk: disk.img

# ── Benchmark binary ────────────────────────────
UCFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-plt -mno-red-zone -nostdlib -O2

$(BUILD)/user/kbench.o: $(SRC)/user/kbench.c | $(BUILD)/user
	$(CC) $(UCFLAGS) -c -o $@ $<

$(BUILD)/user/kbench: $(BUILD)/user/kbench.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

$(SRC)/kernel/kbench_bin.h: $(BUILD)/user/kbench
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated kbench binary (%d bytes) */' % len(data)); \
	print('static const unsigned char kbench_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long kbench_bin_size = %d;' % len(data))" > $@
	@echo "kbench_bin.h: $$(wc -c < $<) bytes"

# Build + boot with benchmark instead of init
bench: $(SRC)/kernel/kbench_bin.h
	@cp $(SRC)/kernel/kbench_bin.h $(SRC)/kernel/init_bin.h.bak
	@sed 's/kbench_bin/init_bin/g; s/kbench_bin_size/init_bin_size/g' \
	  $(SRC)/kernel/kbench_bin.h > $(SRC)/kernel/init_bin.h
	$(MAKE) all
	@rm -f /tmp/cosmo-serial.log
	timeout 300 $(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	  -bios /usr/share/ovmf/OVMF.fd \
	  -drive file=$(ESP_IMG),format=raw \
	  -serial file:/tmp/cosmo-serial.log \
	  -display none -no-reboot \
	  -device e1000,netdev=net0 \
	  -netdev user,id=net0 || true
	@echo "=== Benchmark Results ==="
	@sed -n '/Kernel Benchmark/,/Benchmark Complete/p' /tmp/cosmo-serial.log
	@mv $(SRC)/kernel/init_bin.h.bak $(SRC)/kernel/init_bin.h 2>/dev/null; true

# ── Hardware test binary ─────────────────────────
$(BUILD)/user/ktest.o: $(SRC)/user/ktest.c | $(BUILD)/user
	$(CC) $(UCFLAGS) -c -o $@ $<

$(BUILD)/user/ktest: $(BUILD)/user/ktest.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

$(SRC)/kernel/ktest_bin.h: $(BUILD)/user/ktest
	@python3 -c "\
	data=open('$<','rb').read(); \
	print('/* Auto-generated ktest binary (%d bytes) */' % len(data)); \
	print('static const unsigned char ktest_bin[] = {'); \
	lines = [', '.join('0x%02x'%b for b in data[i:i+16]) for i in range(0,len(data),16)]; \
	print(',\n'.join('    '+l for l in lines)); \
	print('};'); \
	print('static const unsigned long ktest_bin_size = %d;' % len(data))" > $@
	@echo "ktest_bin.h: $$(wc -c < $<) bytes"

# Build + boot with hardware test
test-hw: $(SRC)/kernel/ktest_bin.h
	@cp $(SRC)/kernel/init_bin.h $(SRC)/kernel/init_bin.h.bak 2>/dev/null; true
	@sed 's/ktest_bin/init_bin/g; s/ktest_bin_size/init_bin_size/g' \
	  $(SRC)/kernel/ktest_bin.h > $(SRC)/kernel/init_bin.h
	@rm -f $(BUILD)/kernel/main.o
	$(MAKE) all
	@rm -f /tmp/cosmo-serial.log
	timeout 30 $(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	  -bios /usr/share/ovmf/OVMF.fd \
	  -drive file=$(ESP_IMG),format=raw \
	  -serial file:/tmp/cosmo-serial.log \
	  -display none -no-reboot \
	  -device e1000,netdev=net0 \
	  -netdev user,id=net0 || true
	@echo "=== Hardware Test Results ==="
	@sed -n '/Hardware Test/,/PASSED\|failed/p' /tmp/cosmo-serial.log
	@mv $(SRC)/kernel/init_bin.h.bak $(SRC)/kernel/init_bin.h 2>/dev/null; true

# ── Bootloader (EFI) ────────────────────────────
$(BUILD)/boot/boot.o: $(SRC)/boot/boot.c | $(BUILD)/boot
	$(CC) $(EFI_CFLAGS) -o $@ $<

# ── Kernel ASM ───────────────────────────────────
$(BUILD)/kernel/entry.o: $(SRC)/kernel/entry.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/irq_asm.o: $(SRC)/kernel/irq_asm.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/context.o: $(SRC)/kernel/context.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/syscall_entry.o: $(SRC)/kernel/syscall_entry.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/memops.o: $(SRC)/kernel/memops.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

# ── Kernel C ────────────────────────────────────
$(BUILD)/kernel/%.o: $(SRC)/kernel/%.c | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -o $@ $<

# ── Drivers (same flags, include kernel headers) ──
$(BUILD)/drivers/net/%.o: $(SRC)/drivers/net/%.c | $(BUILD)/drivers/net
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/net -I$(SRC)/drivers/virtio -o $@ $<

$(BUILD)/drivers/blk/%.o: $(SRC)/drivers/blk/%.c | $(BUILD)/drivers/blk
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/blk -I$(SRC)/drivers/virtio -o $@ $<

$(BUILD)/drivers/virtio/%.o: $(SRC)/drivers/virtio/%.c | $(BUILD)/drivers/virtio
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/virtio -o $@ $<

$(BUILD)/drivers/gpu/%.o: $(SRC)/drivers/gpu/%.c | $(BUILD)/drivers/gpu
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/gpu -I$(SRC)/drivers/virtio -o $@ $<

$(BUILD)/drivers/input/%.o: $(SRC)/drivers/input/%.c | $(BUILD)/drivers/input
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/input -I$(SRC)/drivers/virtio -o $@ $<

$(BUILD)/drivers/hyperv/%.o: $(SRC)/drivers/hyperv/%.c | $(BUILD)/drivers/hyperv
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/hyperv -o $@ $<

# main.o depends on init_bin.h, ld_cosmo_bin.h, e1000d_bin.h, svcmgr_bin.h
$(BUILD)/kernel/main.o: $(SRC)/kernel/main.c $(SRC)/kernel/init_bin.h $(SRC)/kernel/ld_cosmo_bin.h $(SRC)/kernel/e1000d_bin.h $(SRC)/kernel/svcmgr_bin.h | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -DHAVE_LD_COSMO -DHAVE_E1000D -DHAVE_SVCMGR -o $@ $<

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
QEMU_FLAGS = -cpu qemu64 -smp 2 -m 4096 \
             -bios /usr/share/ovmf/OVMF.fd \
             -drive file=$(ESP_IMG),format=raw \
             -serial stdio \
             -display none \
             -no-reboot \
             -device e1000,netdev=net0 \
             -netdev user,id=net0

qemu: $(ESP_IMG)
	$(QEMU) $(QEMU_FLAGS)

# VT shell binary (interactive echo on PTY)
$(BUILD)/user/vt_shell.o: $(SRC)/user/vt_shell.c | $(BUILD)/user
	$(CC) $(UCFLAGS) -c -o $@ $<

$(BUILD)/user/vt_shell: $(BUILD)/user/vt_shell.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

qemu-gui: $(BUILD)/user/vt_shell
	@cp $(SRC)/kernel/init_bin.h $(SRC)/kernel/init_bin.h.bak 2>/dev/null; true
	@python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('static const unsigned char init_bin[]={'+','.join(str(b) for b in d)+'};'); print('static const unsigned long init_bin_size=%d;'%len(d))" $(BUILD)/user/vt_shell > $(SRC)/kernel/init_bin.h
	@rm -f $(BUILD)/kernel/main.o
	$(MAKE) all
	@mv $(SRC)/kernel/init_bin.h.bak $(SRC)/kernel/init_bin.h 2>/dev/null; true
	$(QEMU) $(subst -display none,-display gtk,$(subst -no-reboot,,$(QEMU_FLAGS))) -device virtio-keyboard-pci

qemu-disk: $(ESP_IMG) disk.img
	$(QEMU) $(QEMU_FLAGS) -drive file=disk.img,if=virtio,format=raw

qemu-net: $(ESP_IMG)
	$(QEMU) $(QEMU_FLAGS) -device e1000,netdev=net0 -netdev user,id=net0

# Background QEMU with serial log
run: $(ESP_IMG)
	@rm -f /tmp/cosmo-serial.log
	$(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot &

stop:
	@pkill -f "qemu-system.*cosmo-rt" 2>/dev/null; true

# ── Test ────────────────────────────────────────
test-boot: $(ESP_IMG)
	@rm -f /tmp/cosmo-serial.log
	timeout 10 $(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot \
	        -device e1000,netdev=net0 \
	        -netdev user,id=net0 || true
	@echo "=== Serial output ==="
	@cat /tmp/cosmo-serial.log 2>/dev/null || echo "(no output)"
	@grep -q "CosmoOS booted" /tmp/cosmo-serial.log 2>/dev/null && \
	  echo "=== PASS ===" || echo "=== FAIL ==="

test-boot-disk: $(ESP_IMG) disk.img
	@rm -f /tmp/cosmo-serial.log
	timeout 10 $(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -drive file=disk.img,if=virtio,format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot \
	        -device e1000,netdev=net0 \
	        -netdev user,id=net0 || true
	@echo "=== Serial output ==="
	@cat /tmp/cosmo-serial.log 2>/dev/null || echo "(no output)"
	@grep -q "CosmoOS booted" /tmp/cosmo-serial.log 2>/dev/null && \
	  echo "=== PASS ===" || echo "=== FAIL ==="

clean:
	rm -rf $(BUILD)
	rm -f $(SRC)/kernel/init_bin.h $(SRC)/kernel/ap_trampoline_bin.h $(SRC)/kernel/kbench_bin.h $(SRC)/kernel/ktest_bin.h $(SRC)/kernel/ld_cosmo_bin.h $(SRC)/kernel/e1000d_bin.h $(SRC)/kernel/svcmgr_bin.h $(SRC)/kernel/kexec_tramp_bin.h
	rm -f tools/mkfs disk.img
