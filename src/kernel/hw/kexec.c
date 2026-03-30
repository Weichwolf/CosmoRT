/* CosmoRT kexec — kernel hot-swap */

#include "hw/kexec.h"
#include "config.h"
#include "hw/serial.h"
#include "proc/process.h"
#include "core/smp.h"
#include "memops.h"
#include "proc/elf.h"
#include "mm/page_alloc.h"
#include "boot_info.h"
#include "arch/arch.h"
#include "fs/bcache.h"
#include "fs/ext2.h"
#include "sys/syscall.h"
#include "proc/thread.h"
#include "core/irq.h"

#include "uaccess.h"

#include "gen/kexec_tramp_bin.h"

extern uint64_t pml4[];

extern struct boot_info *g_boot_info;

#define LAPIC_ICR_LO  (0xFEE00300ULL + PHYS_OFFSET)

#define TRAMP_PHYS  0x8000ULL
#define TRAMP_DATA  0x8F00ULL

static int validate_elf(const Elf64_Ehdr *eh, size_t len) {
    if (len < sizeof(Elf64_Ehdr)) return -EINVAL;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("kexec: bad ELF magic\n");
        return -ENOEXEC;
    }
    if (eh->e_ident[4] != 2) {
        serial_puts("kexec: not ELF64\n");
        return -ENOEXEC;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_puts("kexec: not x86_64\n");
        return -ENOEXEC;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        serial_puts("kexec: not ET_EXEC/ET_DYN\n");
        return -ENOEXEC;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len) {
        serial_puts("kexec: phdr out of bounds\n");
        return -ENOEXEC;
    }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    int has_load = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) has_load = 1;
        if (ph[i].p_type == PT_LOAD &&
            ph[i].p_offset + ph[i].p_filesz > len) {
            serial_puts("kexec: segment out of bounds\n");
            return -ENOEXEC;
        }
    }
    if (!has_load) {
        serial_puts("kexec: no PT_LOAD\n");
        return -ENOEXEC;
    }

    return 0;
}

static void flush_filesystem(void) {
    serial_puts("kexec: flushing filesystem\n");
    ext2_sync();
    bcache_sync();
}

static void kill_all_processes(void) {
    serial_puts("kexec: killing all processes\n");
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *p = proc_find((uint32_t)i);
        if (!p) continue;

        thread_t *t = p->threads;
        while (t) {
            t->state = THREAD_DEAD;
            t = t->proc_next;
        }
        p->state = PROC_ZOMBIE;
    }
}

static void stop_aps(void) {
    int ncores = smp_num_cores();
    if (ncores <= 1) return;

    serial_puts("kexec: stopping APs (INIT IPI)\n");

    volatile uint32_t *icr_lo = (volatile uint32_t *)LAPIC_ICR_LO;
    *icr_lo = 0x000C4500;

    lapic_delay_ms(10);

    serial_puts("kexec: APs stopped\n");
}

static void __attribute__((noreturn))
load_and_jump(const void *kbuf, size_t len __attribute__((unused))) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kbuf;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)kbuf + eh->e_phoff);

    serial_puts("kexec: loading segments\n");
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        uint64_t dst_phys = ph[i].p_paddr;
        void *dst_virt = phys_to_virt(dst_phys);
        const void *src = (const uint8_t *)kbuf + ph[i].p_offset;

        serial_puts("  seg ");
        serial_hex64(dst_phys);
        serial_puts(" +");
        serial_hex64(ph[i].p_memsz);
        serial_putchar('\n');

        if (ph[i].p_filesz > 0)
            kmemcpy(dst_virt, src, ph[i].p_filesz);

        if (ph[i].p_memsz > ph[i].p_filesz)
            kmemset((uint8_t *)dst_virt + ph[i].p_filesz, 0,
                    ph[i].p_memsz - ph[i].p_filesz);
    }

    uint64_t entry = eh->e_entry;
    serial_puts("kexec: entry=");
    serial_hex64(entry);
    serial_putchar('\n');

    uint64_t boot_info_phys = virt_to_phys(g_boot_info);

    uint64_t pml4_phys = virt_to_phys(pml4);

    uint8_t *tramp_dst = (uint8_t *)phys_to_virt(TRAMP_PHYS);
    kmemcpy(tramp_dst, kexec_tramp_bin, kexec_tramp_bin_size);

    volatile uint64_t *data = (volatile uint64_t *)phys_to_virt(TRAMP_DATA);
    data[0] = entry;
    data[1] = boot_info_phys;
    data[2] = pml4_phys;

    arch_mfence();

    serial_puts("kexec: jumping to trampoline\n");

    arch_cli();

    void (*tramp)(void) = (void (*)(void))phys_to_virt(TRAMP_PHYS);
    tramp();

    __builtin_unreachable();
}

int do_kexec(const void *image, size_t len) {
    if (len == 0 || len > 64 * 1024 * 1024) return -EINVAL;
    serial_puts("kexec: loading new kernel (");
    serial_hex64(len);
    serial_puts(" bytes)\n");

    int npages = (int)((len + 4095) / 4096);
    void *kbuf = pages_alloc(npages);
    if (!kbuf) {
        serial_puts("kexec: out of memory\n");
        return -ENOMEM;
    }
    { int r = copy_from_user(kbuf, image, len); if (r) { pages_free(kbuf, npages); return r; } }

    int err = validate_elf((const Elf64_Ehdr *)kbuf, len);
    if (err) {
        pages_free(kbuf, npages);
        return err;
    }

    flush_filesystem();
    kill_all_processes();
    stop_aps();
    load_and_jump(kbuf, len);
}
