/* CosmoRT kexec — kernel hot-swap
 *
 * Loads a new CosmoRT kernel ELF, stops everything, jumps to it.
 * The new kernel boots fresh (re-initializes all hardware).
 *
 * Sequence:
 *   1. Validate ELF (must be ELF64, x86_64, ET_EXEC)
 *   2. Flush filesystem (bcache + journal)
 *   3. Kill all processes
 *   4. Stop APs via INIT IPI
 *   5. Copy PT_LOAD segments to physical memory
 *   6. Copy trampoline to 0x8000, fill data at 0x8F00
 *   7. Jump to trampoline → new kernel
 */

#include "kexec.h"
#include "config.h"
#include "serial.h"
#include "process.h"
#include "smp.h"
#include "memops.h"
#include "elf.h"
#include "page_alloc.h"
#include "boot_info.h"
#include "bcache.h"
#include "journal.h"
#include "syscall.h"
#include "thread.h"

/* Trampoline binary (assembled from kexec_tramp.asm → flat binary → C header) */
#include "kexec_tramp_bin.h"

/* Kernel PML4 from entry.asm — has identity map in PML4[0..7] */
extern uint64_t pml4[];

/* Boot info saved at kernel startup */
extern struct boot_info *g_boot_info;

/* LAPIC MMIO addresses */
#define LAPIC_ICR_LO  (0xFEE00300ULL + PHYS_OFFSET)

/* Trampoline physical address (same page as AP trampoline, safe to reuse —
 * APs are halted before we touch it) */
#define TRAMP_PHYS  0x8000ULL
#define TRAMP_DATA  0x8F00ULL

/* ── Step 1: Validate ELF ────────────────────────── */

static int validate_elf(const Elf64_Ehdr *eh, size_t len) {
    if (len < sizeof(Elf64_Ehdr)) return -EINVAL;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("kexec: bad ELF magic\n");
        return -ENOEXEC;
    }
    if (eh->e_ident[4] != 2) { /* ELFCLASS64 */
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

    /* Must have at least one PT_LOAD */
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    int has_load = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) has_load = 1;
        /* Bounds check each segment */
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

/* ── Step 2: Flush filesystem ────────────────────── */

static void flush_filesystem(void) {
    serial_puts("kexec: flushing filesystem\n");
    journal_commit();
    bcache_sync();
}

/* ── Step 3: Kill all processes ──────────────────── */

static void kill_all_processes(void) {
    serial_puts("kexec: killing all processes\n");
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &proc_pool[i];
        if (p->state != PROC_ALIVE) continue;

        /* Mark all threads dead */
        thread_t *t = p->threads;
        while (t) {
            t->state = THREAD_DEAD;
            t = t->proc_next;
        }
        p->state = PROC_ZOMBIE;
    }
}

/* ── Step 4: Stop APs ───────────────────────────── */

static void stop_aps(void) {
    int ncores = smp_num_cores();
    if (ncores <= 1) return;

    serial_puts("kexec: stopping APs (INIT IPI)\n");

    /* INIT IPI broadcast: all-excluding-self, INIT assert */
    volatile uint32_t *icr_lo = (volatile uint32_t *)LAPIC_ICR_LO;
    *icr_lo = 0x000C4500; /* shorthand=all-excl-self(0xC0000), INIT(0x4500) */

    /* Wait for APs to enter INIT state */
    for (volatile int i = 0; i < 2000000; i++)
        __asm__ volatile("pause");

    serial_puts("kexec: APs stopped\n");
}

/* ── Step 5-7: Load segments, set up trampoline, jump ── */

static void __attribute__((noreturn))
load_and_jump(const void *kbuf, size_t len __attribute__((unused))) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)kbuf;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)kbuf + eh->e_phoff);

    /* Copy PT_LOAD segments to their physical addresses.
     * CosmoRT kernel is linked at low physical addresses (identity-mapped).
     * We access them via direct map (phys + PHYS_OFFSET). */
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

        /* Copy file data */
        if (ph[i].p_filesz > 0)
            kmemcpy(dst_virt, src, ph[i].p_filesz);

        /* Zero BSS */
        if (ph[i].p_memsz > ph[i].p_filesz)
            kmemset((uint8_t *)dst_virt + ph[i].p_filesz, 0,
                    ph[i].p_memsz - ph[i].p_filesz);
    }

    /* Entry point (physical address, identity-mapped) */
    uint64_t entry = eh->e_entry;
    serial_puts("kexec: entry=");
    serial_hex64(entry);
    serial_putchar('\n');

    /* boot_info: convert back to physical address for the new kernel.
     * The new kernel will add PHYS_OFFSET itself. */
    uint64_t boot_info_phys = virt_to_phys(g_boot_info);

    /* PML4 physical address — the kernel PML4 has both identity map
     * (PML4[0..7]) and direct map (PML4[256..263]). The new kernel
     * expects identity mapping during early boot. */
    uint64_t pml4_phys = virt_to_phys(pml4);

    /* Copy trampoline to 0x8000 */
    uint8_t *tramp_dst = (uint8_t *)phys_to_virt(TRAMP_PHYS);
    kmemcpy(tramp_dst, kexec_tramp_bin, kexec_tramp_bin_size);

    /* Fill data area at 0x8F00 */
    volatile uint64_t *data = (volatile uint64_t *)phys_to_virt(TRAMP_DATA);
    data[0] = entry;           /* +0x00: new kernel entry */
    data[1] = boot_info_phys;  /* +0x08: boot_info physical */
    data[2] = pml4_phys;       /* +0x10: PML4 physical */

    __asm__ volatile("mfence" ::: "memory");

    serial_puts("kexec: jumping to trampoline\n");

    /* Disable interrupts */
    __asm__ volatile("cli");

    /* Jump to trampoline at identity-mapped address.
     * We're currently in direct map (high half), but PML4[0..7] has
     * the same mappings as PML4[256..263], so TRAMP_PHYS is accessible
     * both as 0x8000 and as 0x8000 + PHYS_OFFSET.
     * The trampoline will switch CR3 to use identity map only. */
    void (*tramp)(void) = (void (*)(void))phys_to_virt(TRAMP_PHYS);
    tramp();

    __builtin_unreachable();
}

/* ── Public API ──────────────────────────────────── */

int do_kexec(const void *image, size_t len) {
    serial_puts("kexec: loading new kernel (");
    serial_hex64(len);
    serial_puts(" bytes)\n");

    /* Allocate kernel buffer and copy from userspace */
    int npages = (int)((len + 4095) / 4096);
    void *kbuf = pages_alloc(npages);
    if (!kbuf) {
        serial_puts("kexec: out of memory\n");
        return -ENOMEM;
    }
    kmemcpy(kbuf, image, len);

    /* Validate ELF */
    int err = validate_elf((const Elf64_Ehdr *)kbuf, len);
    if (err) {
        pages_free(kbuf, npages);
        return err;
    }

    /* Point of no return — flush, kill, stop, jump */
    flush_filesystem();
    kill_all_processes();
    stop_aps();
    load_and_jump(kbuf, len); /* does not return */
}
