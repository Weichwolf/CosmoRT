/* CosmoRT ELF Loader — static + dynamic ELF64 binaries
 *
 * elf_load_ex: maps segments (ET_EXEC + ET_DYN), returns elf_info_t
 * elf_load:    thin wrapper: elf_load_ex + build_user_stack
 *
 * ELF-Loader bekommt Bytes, nicht Inodes. Dateisystem-Zugriff ist
 * Aufgabe des Callers (VFS read → Buffer → elf_load_ex). */

#include "proc/proc_internal.h"

/* CSPRNG for ASLR base address selection */
static uint64_t elf_aslr_rand(void) {
    uint64_t r;
    extern int random_get(void *buf, size_t len);
    if (random_get(&r, sizeof(r)) == 0) return r;
    /* Fallback to RDTSC if CSPRNG not yet initialized */
    return arch_rdtsc();
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

        if (ph->p_filesz > len || vaddr + ph->p_filesz < vaddr)
            return 0;

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

/* Map PT_LOAD segments from a CosmoFS inode into user address space.
 * Reads data page-by-page from disk — no large contiguous allocation. */
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
    if (eh->e_phentsize < sizeof(Elf64_Phdr)) { serial_puts("elf_ex: invalid phentsize\n"); return -1; }
    if (eh->e_phnum > 64) { serial_puts("elf_ex: too many phdrs\n"); return -1; }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len) {
        serial_puts("elf_ex: phdr overflow\n"); return -1;
    }

    /* Choose load base */
    uint64_t base = 0;
    if (eh->e_type == ET_DYN) {
        if (base_hint) {
            base = base_hint;
        } else {
            /* ASLR: pick a random base in [0x400000, 0x400000 + 512MB) page-aligned */
            base = 0x400000 + (elf_aslr_rand() & 0x1FFFF000ULL);
        }
    }

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
            }
            break;
        }
    }

    /* Map segments */
    uint64_t brk_end = elf_map_segments(data, len, eh, pml4, base);
    if (brk_end == 0) return -1;

    /* AT_PHDR: virtual address of program headers in mapped memory.
     * Find the PT_LOAD segment that contains file offset e_phoff.
     * phdr vaddr = segment vaddr + (e_phoff - segment file offset). */
    info->prog_phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD &&
            eh->e_phoff >= ph->p_offset &&
            eh->e_phoff < ph->p_offset + ph->p_filesz) {
            info->prog_phdr = base + ph->p_vaddr + (eh->e_phoff - ph->p_offset);
            break;
        }
    }
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

/* ── elf_load: thin wrapper — elf_load_ex + build_user_stack ── */

int elf_load(const void *data, size_t len, uint64_t *user_pml4,
             uint64_t stack_top,
             char kargv[][EXECVE_MAX_STRLEN], int argc,
             char kenvp[][EXECVE_MAX_STRLEN], int envc,
             uint64_t *entry, uint64_t *stack_ptr, uint64_t *brk_out) {
    elf_info_t info;
    if (elf_load_ex(data, len, user_pml4, 0, &info) < 0)
        return -1;

    uint64_t sp = build_user_stack(user_pml4, stack_top,
                                    kargv, argc, kenvp, envc, &info);
    if (!sp) return -1;

    *entry = info.entry;
    *stack_ptr = sp;
    *brk_out = info.brk;
    return 0;
}

