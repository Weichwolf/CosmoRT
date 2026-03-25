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
EFI_LDS  = $(EFI_LIB)/elf_x86_64_efi.lds

EFI_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
             -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
             -Wall -Wextra -Werror -O2 -c \
             -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_INC)/protocol \
             -DGNU_EFI_USE_MS_ABI

KCFLAGS  = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
           -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
           -Wall -Wextra -Werror -O2 -c \
           -Iinclude/public -Iinclude/internal -Iinclude -I$(SRC)/kernel -I$(BUILD) -std=c11

# Drivers: only public headers (cosmo_rt.h) + own subdirectory
DRVFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check -fno-plt \
           -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mgeneral-regs-only \
           -Wall -Wextra -Werror -O2 -c \
           -Iinclude/public -std=c11

LDFLAGS  = -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
           -Bsymbolic -L$(EFI_LIB)

EFI_BIN  = $(BUILD)/BOOTX64.EFI
ESP_IMG  = $(BUILD)/cosmo-rt.img

# Object files
BOOT_OBJ = $(BUILD)/boot/boot.o

# ── ASM objects (kernel root) ──
KERN_ASM = $(BUILD)/kernel/entry.o \
           $(BUILD)/kernel/irq_asm.o \
           $(BUILD)/kernel/context.o \
           $(BUILD)/kernel/syscall_entry.o \
           $(BUILD)/kernel/memops.o

# ── Kernel C objects by subdirectory ──
KERN_CORE = $(BUILD)/kernel/core/main.o \
            $(BUILD)/kernel/core/irq.o \
            $(BUILD)/kernel/core/sched.o \
            $(BUILD)/kernel/core/edf.o \
            $(BUILD)/kernel/core/timer.o \
            $(BUILD)/kernel/core/smp.o \
            $(BUILD)/kernel/core/tss.o \
            $(BUILD)/kernel/core/percpu.o

KERN_MM   = $(BUILD)/kernel/mm/page_alloc.o \
            $(BUILD)/kernel/mm/paging.o \
            $(BUILD)/kernel/mm/vma.o \
            $(BUILD)/kernel/mm/slab.o \
            $(BUILD)/kernel/mm/random.o

KERN_PROC = $(BUILD)/kernel/proc/process.o \
            $(BUILD)/kernel/proc/elf.o

KERN_SYS  = $(BUILD)/kernel/sys/dispatch.o \
             $(BUILD)/kernel/sys/sys_file.o \
             $(BUILD)/kernel/sys/sys_fs.o \
             $(BUILD)/kernel/sys/sys_mem.o \
             $(BUILD)/kernel/sys/sys_proc.o \
             $(BUILD)/kernel/sys/sys_sched.o \
             $(BUILD)/kernel/sys/sys_signal.o \
             $(BUILD)/kernel/sys/sys_time.o \
             $(BUILD)/kernel/sys/sys_ipc.o \
             $(BUILD)/kernel/sys/sys_net.o \
             $(BUILD)/kernel/sys/sys_event.o \
             $(BUILD)/kernel/sys/sys_id.o \
             $(BUILD)/kernel/sys/stubs.o \
             $(BUILD)/kernel/sys/sys_cosmo.o

KERN_IPC  = $(BUILD)/kernel/ipc/ipc.o \
            $(BUILD)/kernel/ipc/futex.o \
            $(BUILD)/kernel/ipc/net_port.o

KERN_FS   = $(BUILD)/kernel/fs/vfs.o \
            $(BUILD)/kernel/fs/cosmofs.o \
            $(BUILD)/kernel/fs/btree.o \
            $(BUILD)/kernel/fs/bcache.o \
            $(BUILD)/kernel/fs/journal.o \
            $(BUILD)/kernel/fs/procfs.o

KERN_NET  = $(BUILD)/kernel/net/net.o \
            $(BUILD)/kernel/net/dispatch.o \
            $(BUILD)/kernel/net/arp.o \
            $(BUILD)/kernel/net/ip.o \
            $(BUILD)/kernel/net/tcp.o \
            $(BUILD)/kernel/net/udp.o \
            $(BUILD)/kernel/net/dns.o \
            $(BUILD)/kernel/net/dhcp.o \
            $(BUILD)/kernel/net/socket.o \
            $(BUILD)/kernel/net/unix_socket.o

KERN_EVENT = $(BUILD)/kernel/event/epoll.o \
             $(BUILD)/kernel/event/eventfd.o \
             $(BUILD)/kernel/event/timerfd.o \
             $(BUILD)/kernel/event/signalfd.o \
             $(BUILD)/kernel/event/inotify.o

KERN_VT   = $(BUILD)/kernel/vt/vt.o \
            $(BUILD)/kernel/vt/pty.o \
            $(BUILD)/kernel/vt/fb.o \
            $(BUILD)/kernel/vt/input.o

KERN_HW   = $(BUILD)/kernel/hw/cosmo_rt.o \
            $(BUILD)/kernel/hw/serial.o \
            $(BUILD)/kernel/hw/serial_bridge.o \
            $(BUILD)/kernel/hw/kexec.o \
            $(BUILD)/arch/x86_64/hyperv.o

KERN_DRV  = $(BUILD)/drivers/virtio/virtio.o \
            $(BUILD)/drivers/virtio/virtio_net.o \
            $(BUILD)/drivers/virtio/virtio_blk.o \
            $(BUILD)/drivers/virtio/virtio_gpu.o \
            $(BUILD)/drivers/virtio/virtio_input.o \
            $(BUILD)/drivers/pci/e1000.o \
            $(BUILD)/drivers/hyperv/vmbus.o \
            $(BUILD)/drivers/hyperv/storvsc.o \
            $(BUILD)/drivers/hyperv/netvsc.o \
            $(BUILD)/drivers/hyperv/hyperv_fb.o \
            $(BUILD)/drivers/hyperv/hv_kbd.o \
            $(BUILD)/drivers/hyperv/hv_mouse.o \
            $(BUILD)/drivers/hyperv/hv_utils.o

KERN_OBJ = $(KERN_ASM) $(KERN_CORE) $(KERN_MM) $(KERN_PROC) \
           $(KERN_SYS) $(KERN_IPC) $(KERN_FS) $(KERN_NET) \
           $(KERN_EVENT) $(KERN_VT) $(KERN_HW) $(KERN_DRV)

ALL_OBJ  = $(BOOT_OBJ) $(KERN_OBJ)

.PHONY: all clean qemu stop init-bin disk vhdx

all: $(ESP_IMG)

# ── Directories ──────────────────────────────────
KDIRS = $(BUILD)/kernel $(BUILD)/kernel/core $(BUILD)/kernel/mm \
        $(BUILD)/kernel/proc $(BUILD)/kernel/ipc \
        $(BUILD)/kernel/fs $(BUILD)/kernel/net $(BUILD)/kernel/event \
        $(BUILD)/kernel/vt $(BUILD)/kernel/hw $(BUILD)/kernel/sys
DDIRS = $(BUILD)/drivers/virtio $(BUILD)/drivers/pci $(BUILD)/drivers/hyperv

$(BUILD)/boot $(KDIRS) $(BUILD)/user $(DDIRS) $(BUILD)/arch/x86_64:
	mkdir -p $@

# ── AP trampoline (16-bit, flat binary → C header) ──
$(BUILD)/kernel/ap_trampoline.bin: $(ARCH_DIR)/ap_trampoline.asm | $(BUILD)/kernel
	$(NASM) -f bin -o $@ $<

$(BUILD)/gen:
	@mkdir -p $@

$(BUILD)/gen/font_atlas.h: fonts/font_atlas.h | $(BUILD)/gen
	@cp $< $@

$(BUILD)/gen/ap_trampoline_bin.h: $(BUILD)/kernel/ap_trampoline.bin | $(BUILD)/gen
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
$(BUILD)/kernel/core/smp.o: $(SRC)/kernel/core/smp.c $(BUILD)/gen/ap_trampoline_bin.h | $(BUILD)/kernel/core
	$(CC) $(KCFLAGS) -I$(SRC)/kernel/gen -o $@ $<

# ── kexec trampoline (64-bit, flat binary → C header) ──
$(BUILD)/kernel/kexec_tramp.bin: $(ARCH_DIR)/kexec_tramp.asm | $(BUILD)/kernel
	$(NASM) -f bin -o $@ $<

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

# ── Init binary (embedded in kernel) ─────────────
$(BUILD)/user/init.o: $(SRC)/user/init.c | $(BUILD)/user
	$(CC) -ffreestanding -fno-stack-protector -fno-stack-check \
	      -fno-plt -mno-red-zone -nostdlib -O2 -c -o $@ $<

$(BUILD)/user/init: $(BUILD)/user/init.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

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

init-bin: $(BUILD)/gen/init_bin.h $(BUILD)/gen/ld_cosmo_bin.h $(BUILD)/gen/e1000d_bin.h $(BUILD)/gen/svcmgr_bin.h

# ── Dynamic linker (ld-cosmo.so, embedded in kernel) ──
$(BUILD)/user/ld-cosmo.o: $(SRC)/user/ld-cosmo.c | $(BUILD)/user
	$(CC) -ffreestanding -fno-stack-protector -fno-stack-check \
	      -fno-plt -mno-red-zone -nostdlib -Wall -Wextra -Werror -O2 -c -o $@ $<

$(BUILD)/user/ld-cosmo: $(BUILD)/user/ld-cosmo.o $(SRC)/user/interp.ld
	$(LD) -T $(SRC)/user/interp.ld -o $@ $<

$(BUILD)/gen/ld_cosmo_bin.h: $(BUILD)/user/ld-cosmo | $(BUILD)/gen
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

$(BUILD)/gen/e1000d_bin.h: $(BUILD)/user/e1000d | $(BUILD)/gen
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

$(BUILD)/gen/svcmgr_bin.h: $(BUILD)/user/svcmgr | $(BUILD)/gen
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

$(BUILD)/disk.img: tools/mkfs $(EFI_BIN) | $(BUILD)
	sh tools/mkimage.sh

disk: $(BUILD)/disk.img

# ── Benchmark binary ────────────────────────────
UCFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-plt -mno-red-zone -nostdlib -O2 \
          -Iinclude/public

$(BUILD)/user/kbench.o: $(SRC)/user/kbench.c | $(BUILD)/user
	$(CC) $(UCFLAGS) -c -o $@ $<

$(BUILD)/user/kbench: $(BUILD)/user/kbench.o $(SRC)/user/init.ld
	$(LD) -T $(SRC)/user/init.ld -o $@ $<

$(BUILD)/gen/kbench_bin.h: $(BUILD)/user/kbench | $(BUILD)/gen
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
bench: $(BUILD)/gen/kbench_bin.h
	@cp $(BUILD)/gen/kbench_bin.h $(BUILD)/gen/init_bin.h.bak
	@sed 's/kbench_bin/init_bin/g; s/kbench_bin_size/init_bin_size/g' \
	  $(BUILD)/gen/kbench_bin.h > $(BUILD)/gen/init_bin.h
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
	@mv $(BUILD)/gen/init_bin.h.bak $(BUILD)/gen/init_bin.h 2>/dev/null; true

# ── Hardware test binary ─────────────────────────
KTEST_SRC = test/main.c $(wildcard test/unit/*.c) $(wildcard test/unit/net/*.c)
KTEST_OBJ = $(BUILD)/test/main.o \
            $(patsubst test/unit/%.c,$(BUILD)/test/unit/%.o,$(wildcard test/unit/*.c)) \
            $(patsubst test/unit/net/%.c,$(BUILD)/test/unit/net/%.o,$(wildcard test/unit/net/*.c))
# test/crash/*.c test/fuzz/*.c disabled

$(BUILD)/test $(BUILD)/test/unit $(BUILD)/test/unit/net $(BUILD)/test/crash $(BUILD)/test/fuzz:
	@mkdir -p $@

$(BUILD)/test/main.o: test/main.c test/ktest.h | $(BUILD)/test
	$(CC) $(UCFLAGS) -Iinclude/internal -Iinclude -Itest -c -o $@ $<

$(BUILD)/test/unit/%.o: test/unit/%.c test/ktest.h | $(BUILD)/test/unit
	$(CC) $(UCFLAGS) -Iinclude/internal -Iinclude -Itest -c -o $@ $<

$(BUILD)/test/unit/net/%.o: test/unit/net/%.c test/ktest.h | $(BUILD)/test/unit/net
	$(CC) $(UCFLAGS) -Iinclude/internal -Iinclude -Itest -c -o $@ $<

$(BUILD)/test/crash/%.o: test/crash/%.c test/ktest.h | $(BUILD)/test/crash
	$(CC) $(UCFLAGS) -Iinclude/internal -Iinclude -Itest -c -o $@ $<

$(BUILD)/test/fuzz/%.o: test/fuzz/%.c test/ktest.h | $(BUILD)/test/fuzz
	$(CC) $(UCFLAGS) -Iinclude/internal -Iinclude -Itest -c -o $@ $<

$(BUILD)/user/ktest: $(KTEST_OBJ) $(SRC)/user/init.ld | $(BUILD)/user
	$(LD) -T $(SRC)/user/init.ld -o $@ $(KTEST_OBJ)

$(BUILD)/gen/ktest_bin.h: $(BUILD)/user/ktest | $(BUILD)/gen
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
test-hw: $(BUILD)/gen/ktest_bin.h
	@cp $(BUILD)/gen/init_bin.h $(BUILD)/gen/init_bin.h.bak 2>/dev/null; true
	$(MAKE) all
	@sed 's/ktest_bin/init_bin/g; s/ktest_bin_size/init_bin_size/g' \
	  $(BUILD)/gen/ktest_bin.h > $(BUILD)/gen/init_bin.h
	@rm -f $(BUILD)/kernel/core/main.o
	$(MAKE) all
	@rm -f /tmp/cosmo-serial.log
	timeout 120 $(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	  -bios /usr/share/ovmf/OVMF.fd \
	  -drive file=$(ESP_IMG),format=raw \
	  -serial file:/tmp/cosmo-serial.log \
	  -display none -no-reboot \
	  -device e1000,netdev=net0 \
	  -netdev user,id=net0 || true
	@echo "=== Hardware Test Results ==="
	@sed -n '/Hardware Test/,/PASSED\|failed/p' /tmp/cosmo-serial.log
	@mv $(BUILD)/gen/init_bin.h.bak $(BUILD)/gen/init_bin.h 2>/dev/null; true

# ── Bootloader (EFI) ────────────────────────────
$(BUILD)/boot/boot.o: $(SRC)/boot/boot.c | $(BUILD)/boot
	$(CC) $(EFI_CFLAGS) -o $@ $<

# ── Architecture ASM (src/arch/x86_64/) ──────────
$(BUILD)/kernel/entry.o: $(ARCH_DIR)/entry.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/irq_asm.o: $(ARCH_DIR)/irq_asm.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/context.o: $(ARCH_DIR)/context.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

$(BUILD)/kernel/syscall_entry.o: $(ARCH_DIR)/syscall_entry.asm | $(BUILD)/kernel
	$(NASM) -f elf64 -o $@ $<

# ── Architecture C (src/arch/x86_64/) ─────────────
$(BUILD)/arch/x86_64/%.o: $(ARCH_DIR)/%.c | $(BUILD)/arch/x86_64
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

# Hyper-V drivers: need hyperv.h from internal/ (kernel-side MSR/SynIC defs)
$(BUILD)/drivers/hyperv/%.o: $(SRC)/drivers/hyperv/%.c | $(BUILD)/drivers/hyperv
	$(CC) $(DRVFLAGS) -I$(SRC)/drivers/hyperv -Iinclude/internal -Iinclude -o $@ $<

# main.o depends on init_bin.h, ld_cosmo_bin.h, e1000d_bin.h, svcmgr_bin.h
$(BUILD)/kernel/core/main.o: $(SRC)/kernel/core/main.c $(BUILD)/gen/init_bin.h $(BUILD)/gen/ld_cosmo_bin.h $(BUILD)/gen/e1000d_bin.h $(BUILD)/gen/svcmgr_bin.h | $(BUILD)/kernel/core
	$(CC) $(KCFLAGS) -I$(SRC)/kernel/gen -DHAVE_LD_COSMO -DHAVE_E1000D -DHAVE_SVCMGR -o $@ $<

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
	@cp $(BUILD)/gen/init_bin.h $(BUILD)/gen/init_bin.h.bak 2>/dev/null; true
	@python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('static const unsigned char init_bin[]={'+','.join(str(b) for b in d)+'};'); print('static const unsigned long init_bin_size=%d;'%len(d))" $(BUILD)/user/vt_shell > $(BUILD)/gen/init_bin.h
	@rm -f $(BUILD)/kernel/core/main.o
	$(MAKE) all
	@mv $(BUILD)/gen/init_bin.h.bak $(BUILD)/gen/init_bin.h 2>/dev/null; true
	$(QEMU) $(subst -display none,-display gtk,$(subst -no-reboot,,$(QEMU_FLAGS))) -device virtio-keyboard-pci

qemu-disk: $(BUILD)/disk.img
	$(QEMU) $(QEMU_FLAGS) \
	        -drive file=build/cosmofs.img,format=raw,if=virtio

# Interactive bash on VT with CosmoFS disk
# COSMO_INTERACTIVE=1 tells mkimage.sh to skip .boot → init starts bash -i
qemu-shell: $(ESP_IMG)
	COSMO_INTERACTIVE=1 sh tools/mkimage.sh
	$(QEMU) $(subst -display none,-display gtk,$(subst -no-reboot,,$(QEMU_FLAGS))) \
	        -drive file=build/cosmofs.img,format=raw,if=virtio \
	        -device virtio-keyboard-pci

vhdx: $(BUILD)/disk.img
	qemu-img convert -f raw -O vhdx $(BUILD)/disk.img $(BUILD)/disk.vhdx
	@echo "disk.vhdx: $$(du -h $(BUILD)/disk.vhdx | cut -f1)"

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

test-boot-disk: $(BUILD)/disk.img
	@rm -f /tmp/cosmo-serial.log
	timeout 10 $(QEMU) -cpu qemu64 -smp 2 -m 4096 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(BUILD)/disk.img,format=raw \
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
	rm -f $(BUILD)/gen/init_bin.h $(BUILD)/gen/ap_trampoline_bin.h $(BUILD)/gen/kbench_bin.h $(BUILD)/gen/ktest_bin.h $(BUILD)/gen/ld_cosmo_bin.h $(BUILD)/gen/e1000d_bin.h $(BUILD)/gen/svcmgr_bin.h $(BUILD)/gen/kexec_tramp_bin.h
	rm -f tools/mkfs
