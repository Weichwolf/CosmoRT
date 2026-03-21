/* CosmoRT ELF Loader — static + dynamic ELF64 binaries
 *
 * elf_load:    original interface, loads ET_EXEC only (backward compat)
 * elf_load_ex: extended, handles ET_EXEC + ET_DYN (PIE/ASLR),
 *              extracts PT_INTERP path, returns metadata for auxv
 */

#include "elf.h"
#include "process.h"
#include "serial.h"
#include "config.h"
#include "page_alloc.h"
#include "memops.h"

/* CSPRNG for ASLR base address selection */
static uint64_t elf_aslr_rand(void) {
    uint64_t r;
    extern int random_get(void *buf, size_t len);
    if (random_get(&r, sizeof(r)) == 0) return r;
    /* Fallback to RDTSC if CSPRNG not yet initialized */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Map PT_LOAD segments from an ELF into user address space.
 * base is added to all p_vaddr (0 for ET_EXEC, ASLR base for ET_DYN).
 * Returns highest loaded address (for brk), or 0 on error. */
static uint64_t elf_map_segments(const void *data, size_t len,
                                 const Elf64_Ehdr *eh, uint64_t *user_pml4,
                                 uint64_t base) {
    uint64_t brk_end = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > len)
            return 0;

        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        uint64_t vaddr = ph->p_vaddr + base;

        serial_puts("  LOAD: vaddr="); serial_hex64(vaddr);
        serial_puts(" filesz="); serial_hex64(ph->p_filesz);
        serial_puts(" memsz="); serial_hex64(ph->p_memsz);
        serial_putchar('\n');

        int seg_prot = 0;
        if (ph->p_flags & PF_R) seg_prot |= PROT_READ;
        if (ph->p_flags & PF_W) seg_prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) seg_prot |= PROT_EXEC;

        uint64_t seg_start = vaddr & ~0xFFFULL;
        uint64_t seg_end = (vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;

        for (uint64_t va = seg_start; va < seg_end; va += 4096) {
            uint64_t *page = alloc_page();
            if (!page) { serial_puts("elf: OOM\n"); return 0; }

            if (va < vaddr + ph->p_filesz) {
                uint64_t page_start = va;
                uint64_t copy_start = page_start < vaddr ? vaddr : page_start;
                uint64_t copy_end = page_start + 4096;
                uint64_t file_end = vaddr + ph->p_filesz;
                if (copy_end > file_end) copy_end = file_end;

                if (copy_start < copy_end) {
                    uint64_t file_off = ph->p_offset + (copy_start - vaddr);
                    uint64_t page_off = copy_start - page_start;
                    uint64_t nbytes = copy_end - copy_start;

                    if (file_off + nbytes <= len)
                        kmemcpy((uint8_t *)page + page_off,
                                (const uint8_t *)data + file_off, nbytes);
                }
            }
            /* BSS (memsz > filesz) already zeroed by alloc_page */

            if (map_user_page(user_pml4, va, virt_to_phys(page), seg_prot) < 0) {
                serial_puts("elf: map failed\n");
                return 0;
            }
        }

        uint64_t seg_data_end = vaddr + ph->p_memsz;
        if (seg_data_end > brk_end) brk_end = seg_data_end;
    }

    return brk_end ? brk_end : 1; /* return nonzero on success */
}

/* ── elf_load_ex: extended loader for ET_EXEC + ET_DYN ─────── */

int elf_load_ex(const void *data, size_t len, uint64_t *pml4,
                uint64_t base_hint, elf_info_t *info) {
    if (len < sizeof(Elf64_Ehdr)) return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Validate ELF magic */
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("elf_ex: bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != 2) { serial_puts("elf_ex: not 64-bit\n"); return -1; }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        serial_puts("elf_ex: unsupported e_type\n");
        return -1;
    }
    if (eh->e_machine != EM_X86_64) { serial_puts("elf_ex: not x86_64\n"); return -1; }

    /* Choose load base */
    uint64_t base = 0;
    if (eh->e_type == ET_DYN) {
        if (base_hint) {
            base = base_hint;
        } else {
            /* ASLR: pick a random base in [0x400000, 0x400000 + 512MB) page-aligned */
            base = 0x400000 + (elf_aslr_rand() & 0x1FFFF000ULL);
        }
        serial_puts("elf_ex: ET_DYN base="); serial_hex64(base); serial_putchar('\n');
    } else {
        serial_puts("elf_ex: ET_EXEC entry="); serial_hex64(eh->e_entry); serial_putchar('\n');
    }

    serial_puts("elf_ex: phnum="); serial_putchar('0' + eh->e_phnum / 10);
    serial_putchar('0' + eh->e_phnum % 10); serial_putchar('\n');

    /* Scan for PT_INTERP before mapping */
    info->interp[0] = '\0';
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > len)
            break;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_INTERP) {
            /* Read interpreter path from ELF data */
            size_t plen = ph->p_filesz;
            if (plen >= sizeof(info->interp)) plen = sizeof(info->interp) - 1;
            if (ph->p_offset + plen <= len) {
                kmemcpy(info->interp, (const uint8_t *)data + ph->p_offset, plen);
                info->interp[plen] = '\0';
                serial_puts("elf_ex: interp=");
                serial_puts(info->interp);
                serial_putchar('\n');
            }
            break;
        }
    }

    /* Map segments */
    uint64_t brk_end = elf_map_segments(data, len, eh, pml4, base);
    if (brk_end == 0) return -1;

    /* Compute program header address in mapped memory.
     * If a PT_LOAD segment covers offset 0 (contains ELF header + phdrs),
     * phdrs are at base + e_phoff. For ET_EXEC, base=0 so it's just e_phoff
     * which should be within the first PT_LOAD's vaddr range. */
    info->prog_phdr = base + eh->e_phoff;
    info->prog_phent = eh->e_phentsize;
    info->prog_phnum = eh->e_phnum;
    info->prog_entry = base + eh->e_entry;
    info->entry = info->prog_entry; /* overridden if interpreter loaded */
    info->interp_base = 0;
    info->brk = (brk_end + 0xFFF) & ~0xFFFULL;
    info->stack_ptr = 0; /* caller sets up stack */
    info->load_base = base;

    return 0;
}

/* ── elf_load: original interface (backward compat) ────────── */

int elf_load(const void *data, size_t len, uint64_t *user_pml4,
             uint64_t stack_top,
             uint64_t *entry, uint64_t *stack_ptr, uint64_t *brk_out) {
    if (len < sizeof(Elf64_Ehdr)) return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Validate ELF magic */
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("elf: bad magic\n");
        return -1;
    }

    /* Must be 64-bit executable for x86_64 */
    if (eh->e_ident[4] != 2) { serial_puts("elf: not 64-bit\n"); return -1; }
    if (eh->e_type != ET_EXEC) { serial_puts("elf: not ET_EXEC\n"); return -1; }
    if (eh->e_machine != EM_X86_64) { serial_puts("elf: not x86_64\n"); return -1; }

    serial_puts("elf: entry="); serial_hex64(eh->e_entry);
    serial_puts(" phnum="); serial_putchar('0' + eh->e_phnum / 10);
    serial_putchar('0' + eh->e_phnum % 10); serial_putchar('\n');

    uint64_t brk_end = elf_map_segments(data, len, eh, user_pml4, 0);
    if (brk_end == 0) return -1;

    /* Set up user stack (map 4 pages at top initially, 16KB) */
    for (int i = 0; i < 4; i++) {
        uint64_t va = stack_top - (uint64_t)(i + 1) * 4096;
        uint64_t *page = alloc_page();
        if (!page) { serial_puts("elf: stack OOM\n"); return -1; }
        if (map_user_page(user_pml4, va, virt_to_phys(page), PROT_READ | PROT_WRITE) < 0)
            return -1;
    }

    /* Build initial stack: argc, argv, envp, auxv */
    uint64_t stack_frame_va = stack_top - 4096;
    uint64_t *frame_page = alloc_page();
    if (!frame_page) return -1;
    /* Remap this page (overwrites the previous mapping, which is fine) */
    map_user_page(user_pml4, stack_frame_va, virt_to_phys(frame_page), PROT_READ | PROT_WRITE);

    uint64_t *stk = (uint64_t *)((uint8_t *)frame_page + 4096);

    /* auxv */
    *(--stk) = 0;              /* AT_NULL value */
    *(--stk) = AT_NULL;        /* AT_NULL type */
    *(--stk) = 4096;           /* AT_PAGESZ value */
    *(--stk) = AT_PAGESZ;     /* AT_PAGESZ type */
    *(--stk) = eh->e_entry;   /* AT_ENTRY value */
    *(--stk) = AT_ENTRY;      /* AT_ENTRY type */
    *(--stk) = eh->e_phentsize; /* AT_PHENT value */
    *(--stk) = AT_PHENT;      /* AT_PHENT type */
    *(--stk) = eh->e_phnum;   /* AT_PHNUM value */
    *(--stk) = AT_PHNUM;      /* AT_PHNUM type */
    /* envp */
    *(--stk) = 0;             /* envp[0] = NULL */
    /* argv */
    *(--stk) = 0;             /* argv[0] = NULL */
    /* argc */
    *(--stk) = 0;             /* argc = 0 */

    uint64_t sp = stack_frame_va + (uint64_t)((uint8_t *)stk - (uint8_t *)frame_page);
    /* Align to 16 bytes then subtract 8: GCC _start expects RSP = 16n+8 */
    sp = (sp & ~0xFULL) - 8;

    *entry = eh->e_entry;
    *stack_ptr = sp;
    *brk_out = (brk_end + 0xFFF) & ~0xFFFULL;

    serial_puts("elf: loaded, sp="); serial_hex64(sp); serial_putchar('\n');
    return 0;
}
