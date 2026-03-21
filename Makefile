# CosmoRT Build System

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
           $(BUILD)/kernel/vfs.o \
           $(BUILD)/kernel/hw.o \
           $(BUILD)/kernel/net.o \
           $(BUILD)/drivers/net/e1000.o \
           $(BUILD)/kernel/socket.o

ALL_OBJ  = $(BOOT_OBJ) $(KERN_OBJ)

.PHONY: all clean qemu stop init-bin

all: $(ESP_IMG)

# ── Directories ──────────────────────────────────
$(BUILD)/boot $(BUILD)/kernel $(BUILD)/user $(BUILD)/drivers/net:
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

init-bin: $(SRC)/kernel/init_bin.h

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
	timeout 300 $(QEMU) -cpu qemu64 -smp 1 -m 256 \
	  -bios /usr/share/ovmf/OVMF.fd \
	  -drive file=$(ESP_IMG),format=raw \
	  -serial file:/tmp/cosmo-serial.log \
	  -display none -no-reboot || true
	@echo "=== Benchmark Results ==="
	@sed -n '/Kernel Benchmark/,/Benchmark Complete/p' /tmp/cosmo-serial.log
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
	$(CC) $(KCFLAGS) -I$(SRC)/drivers/net -o $@ $<

# main.o depends on init_bin.h
$(BUILD)/kernel/main.o: $(SRC)/kernel/main.c $(SRC)/kernel/init_bin.h | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -o $@ $<

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
QEMU_FLAGS = -cpu qemu64 -smp 1 -m 256 \
             -bios /usr/share/ovmf/OVMF.fd \
             -drive file=$(ESP_IMG),format=raw \
             -serial stdio \
             -display none \
             -no-reboot

qemu: $(ESP_IMG)
	$(QEMU) $(QEMU_FLAGS)

qemu-net: $(ESP_IMG)
	$(QEMU) $(QEMU_FLAGS) -device e1000,netdev=net0 -netdev user,id=net0

# Background QEMU with serial log
run: $(ESP_IMG)
	@rm -f /tmp/cosmo-serial.log
	$(QEMU) -cpu qemu64 -smp 1 -m 256 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot &

stop:
	@pkill -f "qemu-system.*cosmo-rt" 2>/dev/null; true

# ── Test ────────────────────────────────────────
test-boot: $(ESP_IMG)
	@rm -f /tmp/cosmo-serial.log
	timeout 10 $(QEMU) -cpu qemu64 -smp 1 -m 256 \
	        -bios /usr/share/ovmf/OVMF.fd \
	        -drive file=$(ESP_IMG),format=raw \
	        -serial file:/tmp/cosmo-serial.log \
	        -display none -no-reboot || true
	@echo "=== Serial output ==="
	@cat /tmp/cosmo-serial.log 2>/dev/null || echo "(no output)"
	@grep -q "CosmoOS booted" /tmp/cosmo-serial.log 2>/dev/null && \
	  echo "=== PASS ===" || echo "=== FAIL ==="

clean:
	rm -rf $(BUILD)
	rm -f $(SRC)/kernel/init_bin.h $(SRC)/kernel/ap_trampoline_bin.h $(SRC)/kernel/kbench_bin.h
