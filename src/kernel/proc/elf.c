/* CosmoRT ELF Loader — static + dynamic ELF64 binaries */

#include "proc/proc_internal.h"

static uint64_t elf_aslr_rand(void) {
    uint64_t r;
    extern int random_get(void *buf, size_t len);
    if (random_get(&r, sizeof(r)) == 0) return r;
    return arch_rdtsc();
}

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

            if (map_user_page(user_pml4, va, virt_to_phys(page), seg_prot) < 0) {
                serial_puts("elf: map failed\n");
                return 0;
            }
        }

        uint64_t seg_data_end = vaddr + ph->p_memsz;
        if (seg_data_end > brk_end) brk_end = seg_data_end;
    }

    return brk_end ? brk_end : 1;
}

int elf_load_ex(const void *data, size_t len, uint64_t *pml4,
                uint64_t base_hint, elf_info_t *info) {
    if (len < sizeof(Elf64_Ehdr)) return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

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

    uint64_t base = 0;
    if (eh->e_type == ET_DYN) {
        if (base_hint) {
            base = base_hint;
        } else {
            base = 0x400000 + (elf_aslr_rand() & 0x1FFFF000ULL);
        }
    }

    info->interp[0] = '\0';
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > len)
            break;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_INTERP) {
            size_t plen = ph->p_filesz;
            if (plen >= sizeof(info->interp)) plen = sizeof(info->interp) - 1;
            if (ph->p_offset + plen <= len) {
                kmemcpy(info->interp, (const uint8_t *)data + ph->p_offset, plen);
                info->interp[plen] = '\0';
            }
            break;
        }
    }

    uint64_t brk_end = elf_map_segments(data, len, eh, pml4, base);
    if (brk_end == 0) return -1;

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
    info->entry = info->prog_entry;
    info->interp_base = 0;
    info->brk = (brk_end + 0xFFF) & ~0xFFFULL;
    info->stack_ptr = 0;
    info->load_base = base;

    return 0;
}

typedef int (*elf_reader_fn)(void *ctx, void *buf, size_t offset, size_t len);

static int elf_read_ext2(void *ctx, void *buf, size_t offset, size_t len) {
    uint32_t ino = (uint32_t)(uintptr_t)ctx;
    extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
    return ext2_read(ino, buf, offset, len);
}

static int elf_read_ramfs(void *ctx, void *buf, size_t offset, size_t len) {
    uint64_t *pair = (uint64_t *)ctx;
    uint8_t *data = (uint8_t *)pair[0];
    size_t size = (size_t)pair[1];
    if (offset >= size) return 0;
    if (offset + len > size) len = size - offset;
    kmemcpy(buf, data + offset, len);
    return (int)len;
}

static uint64_t elf_map_segments_stream(const Elf64_Ehdr *eh, size_t hdr_len,
                                        size_t file_size, uint64_t *user_pml4,
                                        uint64_t base,
                                        elf_reader_fn reader, void *ctx) {
    uint64_t brk_end = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        size_t ph_off = (size_t)(eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph_off + sizeof(Elf64_Phdr) > hdr_len)
            return 0;

        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)eh + ph_off);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        uint64_t vaddr = ph->p_vaddr + base;

        if (ph->p_filesz > file_size || vaddr + ph->p_filesz < vaddr)
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

                    int rd = reader(ctx, (uint8_t *)page + page_off,
                                    (size_t)file_off, (size_t)nbytes);
                    if (rd < (int)nbytes) {
                        serial_puts("elf: short read\n");
                        return 0;
                    }
                }
            }

            if (map_user_page(user_pml4, va, virt_to_phys(page), seg_prot) < 0) {
                serial_puts("elf: map failed\n");
                return 0;
            }
        }

        uint64_t seg_data_end = vaddr + ph->p_memsz;
        if (seg_data_end > brk_end) brk_end = seg_data_end;
    }

    return brk_end ? brk_end : 1;
}

int elf_load_ex_stream(elf_reader_fn reader, void *ctx, size_t file_size,
                       uint64_t *pml4, uint64_t base_hint, elf_info_t *info) {
    uint8_t *hdr_page = (uint8_t *)alloc_page();
    if (!hdr_page) return -1;

    if (reader(ctx, hdr_page, 0, sizeof(Elf64_Ehdr)) < (int)sizeof(Elf64_Ehdr)) {
        page_free(hdr_page);
        return -1;
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)hdr_page;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("elf_stream: bad magic\n");
        page_free(hdr_page);
        return -1;
    }
    if (eh->e_ident[4] != 2) { page_free(hdr_page); return -1; }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        page_free(hdr_page);
        return -1;
    }
    if (eh->e_machine != EM_X86_64) { page_free(hdr_page); return -1; }
    if (eh->e_phentsize < sizeof(Elf64_Phdr)) { page_free(hdr_page); return -1; }
    if (eh->e_phnum > 64) { page_free(hdr_page); return -1; }

    size_t phdrs_size = (size_t)eh->e_phnum * eh->e_phentsize;
    size_t hdr_total = (size_t)eh->e_phoff + phdrs_size;
    if (hdr_total > 4096) {
        serial_puts("elf_stream: phdrs too large\n");
        page_free(hdr_page);
        return -1;
    }
    if (eh->e_phoff > 0) {
        size_t read_start = sizeof(Elf64_Ehdr);
        size_t read_len = hdr_total - read_start;
        if (read_len > 0) {
            int rd = reader(ctx, hdr_page + read_start, read_start, read_len);
            if (rd < (int)read_len) {
                page_free(hdr_page);
                return -1;
            }
        }
    }

    uint64_t base = 0;
    if (eh->e_type == ET_DYN) {
        if (base_hint) {
            base = base_hint;
        } else {
            base = 0x400000 + (elf_aslr_rand() & 0x1FFFF000ULL);
        }
    }

    info->interp[0] = '\0';
    for (int i = 0; i < eh->e_phnum; i++) {
        size_t ph_off = (size_t)(eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph_off + sizeof(Elf64_Phdr) > hdr_total) break;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(hdr_page + ph_off);
        if (ph->p_type == PT_INTERP) {
            size_t plen = ph->p_filesz;
            if (plen >= sizeof(info->interp)) plen = sizeof(info->interp) - 1;
            int rd = reader(ctx, info->interp, (size_t)ph->p_offset, plen);
            if (rd < (int)plen) {
                info->interp[0] = '\0';
            } else {
                info->interp[plen] = '\0';
            }
            break;
        }
    }

    uint64_t brk_end = elf_map_segments_stream(eh, hdr_total, file_size,
                                                pml4, base, reader, ctx);
    if (brk_end == 0) {
        page_free(hdr_page);
        return -1;
    }

    info->prog_phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        size_t ph_off = (size_t)(eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(hdr_page + ph_off);
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
    info->entry = info->prog_entry;
    info->interp_base = 0;
    info->brk = (brk_end + 0xFFF) & ~0xFFFULL;
    info->stack_ptr = 0;
    info->load_base = base;

    page_free(hdr_page);
    return 0;
}

int elf_load_ext2(uint32_t ino, size_t file_size, uint64_t *pml4,
                  uint64_t base_hint, elf_info_t *info) {
    return elf_load_ex_stream(elf_read_ext2, (void *)(uintptr_t)ino,
                              file_size, pml4, base_hint, info);
}

int elf_load_ramfs(uint8_t *data, size_t size, uint64_t *pml4,
                   uint64_t base_hint, elf_info_t *info) {
    uint64_t pair[2] = { (uint64_t)(uintptr_t)data, (uint64_t)size };
    return elf_load_ex_stream(elf_read_ramfs, pair,
                              size, pml4, base_hint, info);
}

int elf_load(const void *data, size_t len, uint64_t *user_pml4,
             uint64_t stack_top,
             const char *const *argv, int argc,
             const char *const *envp, int envc,
             uint64_t *entry, uint64_t *stack_ptr, uint64_t *brk_out) {
    elf_info_t info;
    if (elf_load_ex(data, len, user_pml4, 0, &info) < 0)
        return -1;

    uint64_t sp = build_user_stack(user_pml4, stack_top,
                                    argv, argc, envp, envc, &info);
    if (!sp) return -1;

    *entry = info.entry;
    *stack_ptr = sp;
    *brk_out = info.brk;
    return 0;
}
