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
    /* Fallback to TSC if CSPRNG not yet initialized */
    return hal_cpu_timestamp();
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

/* ── Streaming ELF loader — reads from ext4/ramfs page-by-page ── */

/* Reader abstraction: read `len` bytes at `offset` into `buf`.
 * Returns bytes read (>= 0) or negative errno. */
typedef int (*elf_reader_fn)(void *ctx, void *buf, size_t offset, size_t len);

static int elf_read_ext4(void *ctx, void *buf, size_t offset, size_t len) {
    uint32_t ino = (uint32_t)(uintptr_t)ctx;
    extern int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
    return ext4_read(ino, buf, offset, len);
}

static int elf_read_ramfs(void *ctx, void *buf, size_t offset, size_t len) {
    /* ctx is a struct vfs_inode *; reader serializes against truncate via
     * inode->lock inside vfs_inode_read. */
    struct vfs_inode *inode = (struct vfs_inode *)ctx;
    extern size_t vfs_inode_read(struct vfs_inode *, void *, size_t, size_t);
    return (int)vfs_inode_read(inode, buf, offset, len);
}

/* Map PT_LOAD segments by reading page-by-page from a reader function.
 * `hdr_buf` contains the ELF header + program headers (already read).
 * `hdr_len` is the size of hdr_buf.
 * `file_size` is the total file size.
 * Returns highest loaded address (for brk), or 0 on error. */
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
            /* BSS (memsz > filesz) already zeroed by alloc_page */

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

/* Streaming elf_load_ex: reads only ELF header + phdrs, then maps
 * segments page-by-page via reader function. No large contiguous buffer. */
int elf_load_ex_stream(elf_reader_fn reader, void *ctx, size_t file_size,
                       uint64_t *pml4, uint64_t base_hint, elf_info_t *info) {
    /* Read ELF header + all program headers into a single page.
     * Max: 64 bytes (ehdr) + 64 * 56 bytes (phdrs) = 3648 bytes < 4096. */
    uint8_t *hdr_page = (uint8_t *)alloc_page();
    if (!hdr_page) return -1;

    /* Read header first */
    if (reader(ctx, hdr_page, 0, sizeof(Elf64_Ehdr)) < (int)sizeof(Elf64_Ehdr)) {
        page_free(hdr_page);
        return -1;
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)hdr_page;

    /* Validate ELF magic */
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

    /* Read program headers */
    size_t phdrs_size = (size_t)eh->e_phnum * eh->e_phentsize;
    size_t hdr_total = (size_t)eh->e_phoff + phdrs_size;
    if (hdr_total > 4096) {
        /* Extremely rare: phdrs don't fit in one page. Bail out. */
        serial_puts("elf_stream: phdrs too large\n");
        page_free(hdr_page);
        return -1;
    }
    if (eh->e_phoff > 0) {
        size_t read_start = sizeof(Elf64_Ehdr); /* already have this */
        size_t read_len = hdr_total - read_start;
        if (read_len > 0) {
            int rd = reader(ctx, hdr_page + read_start, read_start, read_len);
            if (rd < (int)read_len) {
                page_free(hdr_page);
                return -1;
            }
        }
    }

    /* Choose load base */
    uint64_t base = 0;
    if (eh->e_type == ET_DYN) {
        if (base_hint) {
            base = base_hint;
        } else {
            base = 0x400000 + (elf_aslr_rand() & 0x1FFFF000ULL);
        }
    }

    /* Scan for PT_INTERP */
    info->interp[0] = '\0';
    for (int i = 0; i < eh->e_phnum; i++) {
        size_t ph_off = (size_t)(eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph_off + sizeof(Elf64_Phdr) > hdr_total) break;
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(hdr_page + ph_off);
        if (ph->p_type == PT_INTERP) {
            size_t plen = ph->p_filesz;
            if (plen >= sizeof(info->interp)) plen = sizeof(info->interp) - 1;
            /* Read interp path from file (may not be in hdr_page) */
            int rd = reader(ctx, info->interp, (size_t)ph->p_offset, plen);
            if (rd < (int)plen) {
                info->interp[0] = '\0';
            } else {
                info->interp[plen] = '\0';
            }
            break;
        }
    }

    /* Map segments page-by-page */
    uint64_t brk_end = elf_map_segments_stream(eh, hdr_total, file_size,
                                                pml4, base, reader, ctx);
    if (brk_end == 0) {
        page_free(hdr_page);
        return -1;
    }

    /* AT_PHDR */
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

/* Public wrappers for streaming loader */

int elf_load_ext4(uint32_t ino, size_t file_size, uint64_t *pml4,
                  uint64_t base_hint, elf_info_t *info) {
    return elf_load_ex_stream(elf_read_ext4, (void *)(uintptr_t)ino,
                              file_size, pml4, base_hint, info);
}

int elf_load_ramfs(struct vfs_inode *inode, size_t size, uint64_t *pml4,
                   uint64_t base_hint, elf_info_t *info) {
    return elf_load_ex_stream(elf_read_ramfs, (void *)inode,
                              size, pml4, base_hint, info);
}

/* ── elf_load: thin wrapper — elf_load_ex + build_user_stack ── */

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

