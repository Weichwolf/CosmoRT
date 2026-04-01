/* CosmoRT Process — execve */

#include "proc/proc_internal.h"

/* ── execve helpers ──────────────────────────────── */

/* Copy user path string to kernel buffer.
 * Duplicated here because process_exec.c is a separate compilation unit. */
#define PATH_MAX_PROC 4096
static int copy_path_from_user_proc(char *kbuf, const char *upath, size_t max) {
    if ((uint64_t)upath >= 0x800000000000ULL) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
        if ((uint64_t)(upath + i) >= 0x800000000000ULL) return -EFAULT;
        kbuf[i] = upath[i];
        if (kbuf[i] == '\0') return (int)i;
    }
    return -ENAMETOOLONG;
}

/* Build user stack with argv, envp, auxv. Allocates stack pages.
 * Returns RSP value on success, 0 on error. */
uint64_t build_user_stack(uint64_t *user_pml4, uint64_t stack_top,
                                 const char *const *argv, int argc,
                                 const char *const *envp, int envc,
                                 const elf_info_t *elf_info) {
    /* Map stack pages — enough for strings + metadata.
     * 8 pages (32KB) covers typical gcc/ld invocations. */
    #define STK_PAGES 8
    uint8_t *stk_kern[STK_PAGES]; /* kernel-mapped page pointers */
    uint64_t stk_base_va = stack_top - (uint64_t)STK_PAGES * 4096;
    for (int i = 0; i < STK_PAGES; i++) {
        uint64_t *pg = alloc_page();
        if (!pg) return 0;
        uint64_t va = stk_base_va + (uint64_t)i * 4096;
        if (map_user_page(user_pml4, va, virt_to_phys(pg), PROT_READ | PROT_WRITE) < 0)
            return 0;
        stk_kern[i] = (uint8_t *)pg;
    }

    /* Helper: write byte at offset within multi-page stack area.
     * off is relative to stk_base_va. */
    #define STK_SIZE ((uint64_t)STK_PAGES * 4096)
    #define STK_BYTE(off) (stk_kern[(off) >> 12][(off) & 0xFFF])

    uint64_t str_off = STK_SIZE; /* write top-down */
    uint64_t argv_addrs[EXECVE_MAX_ARGS];
    uint64_t envp_addrs[EXECVE_MAX_ENVS];

    /* 16 random bytes for AT_RANDOM */
    str_off -= 16;
    str_off &= ~7ULL;
    uint64_t at_random_addr = stk_base_va + str_off;
    extern int random_get(void *buf, size_t len);
    /* Write AT_RANDOM bytes page-aware */
    {
        uint8_t rnd[16];
        if (random_get(rnd, 16) != 0)
            kmemset(rnd, 0x42, 16);
        for (int i = 0; i < 16; i++)
            STK_BYTE(str_off + (uint64_t)i) = rnd[i];
    }

    /* Environment strings */
    for (int i = envc - 1; i >= 0; i--) {
        int sl = 0; while (envp[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) return 0;
        str_off -= (uint64_t)(sl + 1);
        for (int j = 0; j <= sl; j++)
            STK_BYTE(str_off + (uint64_t)j) = (uint8_t)envp[i][j];
        envp_addrs[i] = stk_base_va + str_off;
    }
    /* Argument strings */
    for (int i = argc - 1; i >= 0; i--) {
        int sl = 0; while (argv[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) return 0;
        str_off -= (uint64_t)(sl + 1);
        for (int j = 0; j <= sl; j++)
            STK_BYTE(str_off + (uint64_t)j) = (uint8_t)argv[i][j];
        argv_addrs[i] = stk_base_va + str_off;
    }

    str_off &= ~7ULL;

    /* Count qwords: argc(1) + argv(argc+1) + envp(envc+1) + auxv(8*2+2) */
    int naux = 8; /* PHDR, PHENT, PHNUM, BASE, ENTRY, PAGESZ, RANDOM, NULL */
    int nqwords = 1 + (argc + 1) + (envc + 1) + (naux * 2);
    str_off -= (uint64_t)nqwords * 8;
    str_off &= ~0xFULL; /* 16-byte align RSP at process entry */

    /* Write metadata (argc, argv, envp, auxv) as qwords */
    uint64_t wp = str_off;
    #define STK_QWORD(off, val) do { \
        uint64_t _v = (val); \
        for (int _b = 0; _b < 8; _b++) \
            STK_BYTE((off) + (uint64_t)_b) = (uint8_t)(_v >> (_b * 8)); \
    } while(0)

    /* argc */
    STK_QWORD(wp, (uint64_t)argc); wp += 8;
    /* argv pointers */
    for (int i = 0; i < argc; i++) { STK_QWORD(wp, argv_addrs[i]); wp += 8; }
    STK_QWORD(wp, 0); wp += 8; /* argv terminator */
    /* envp pointers */
    for (int i = 0; i < envc; i++) { STK_QWORD(wp, envp_addrs[i]); wp += 8; }
    STK_QWORD(wp, 0); wp += 8; /* envp terminator */
    /* auxv */
    STK_QWORD(wp, AT_PHDR);   wp += 8; STK_QWORD(wp, elf_info->prog_phdr);          wp += 8;
    STK_QWORD(wp, AT_PHENT);  wp += 8; STK_QWORD(wp, (uint64_t)elf_info->prog_phent); wp += 8;
    STK_QWORD(wp, AT_PHNUM);  wp += 8; STK_QWORD(wp, (uint64_t)elf_info->prog_phnum); wp += 8;
    STK_QWORD(wp, AT_BASE);   wp += 8; STK_QWORD(wp, elf_info->interp_base);         wp += 8;
    STK_QWORD(wp, AT_ENTRY);  wp += 8; STK_QWORD(wp, elf_info->prog_entry);          wp += 8;
    STK_QWORD(wp, AT_PAGESZ); wp += 8; STK_QWORD(wp, 4096);                          wp += 8;
    STK_QWORD(wp, AT_RANDOM); wp += 8; STK_QWORD(wp, at_random_addr);                wp += 8;
    STK_QWORD(wp, AT_NULL);   wp += 8; STK_QWORD(wp, 0);                             wp += 8;

    #undef STK_BYTE
    #undef STK_QWORD
    #undef STK_SIZE
    #undef STK_PAGES

    return stk_base_va + str_off;
}

/* Create VMAs for mapped ELF segments (using elf_info_t metadata).
 * base is the load base used (0 for ET_EXEC). */
static void create_elf_vmas(vma_t **vma_root, const void *elf_data,
                            size_t elf_len, uint64_t base) {
    if (elf_len < 64) return;
    const uint8_t *data = (const uint8_t *)elf_data;
    uint64_t phoff = *(const uint64_t *)(data + 32);
    uint16_t phentsize = *(const uint16_t *)(data + 54);
    uint16_t phnum = *(const uint16_t *)(data + 56);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = data + phoff + (uint64_t)i * phentsize;
        uint32_t p_type = *(const uint32_t *)ph;
        if (p_type != 1) continue; /* PT_LOAD */
        uint64_t p_vaddr = *(const uint64_t *)(ph + 16) + base;
        uint64_t p_memsz = *(const uint64_t *)(ph + 40);
        uint32_t p_flags = *(const uint32_t *)(ph + 4);
        if (p_memsz == 0) continue;
        uint64_t seg_start = p_vaddr & ~0xFFFULL;
        uint64_t seg_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
        int prot = 0;
        if (p_flags & 4) prot |= PROT_READ;
        if (p_flags & 2) prot |= PROT_WRITE;
        if (p_flags & 1) prot |= PROT_EXEC;
        vma_insert(vma_root, seg_start, seg_end, prot, MAP_PRIVATE);
    }
}

/* ── execve (2.2) ────────────────────────────────── */

extern void proc_enter_ring3(thread_t *t) __attribute__((noreturn));

long do_execve(const char *path, char *const argv[], char *const envp[]) {
    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *p = cur->proc;

    /* Copy path to kernel buffer and resolve relative paths via CWD */
    char kpath_raw[PATH_MAX_PROC], kpath[PATH_MAX_PROC];
    int plen = copy_path_from_user_proc(kpath_raw, path, PATH_MAX_PROC);
    if (plen < 0) return -EFAULT;
    extern int resolve_path(const char *path, char *out, int outsize);
    resolve_path(kpath_raw, kpath, PATH_MAX_PROC);

    /* Copy argv/envp from userspace into a flat page-allocated buffer.
     * Layout: string data packed contiguously, pointer arrays on stack. */
    #define EXECVE_BUF_PAGES (EXECVE_BUF_SIZE / 4096)
    char *strbuf = (char *)pages_alloc(EXECVE_BUF_PAGES);
    if (!strbuf) return -ENOMEM;
    size_t buf_off = 0;

    const char *kargv_ptrs[EXECVE_MAX_ARGS];
    int argc = 0;
    if (argv && (uint64_t)argv < 0x800000000000ULL) {
        for (int i = 0; i < EXECVE_MAX_ARGS; i++) {
            char *const *ap = &argv[i];
            if ((uint64_t)ap + sizeof(char *) > 0x800000000000ULL) break;
            char *arg = *ap;
            if (!arg) break;
            if ((uint64_t)arg >= 0x800000000000ULL) break;
            size_t avail = EXECVE_BUF_SIZE - buf_off;
            if (avail < 2) break;
            int r = copy_path_from_user_proc(strbuf + buf_off, arg,
                        avail < (size_t)EXECVE_MAX_STRLEN ? avail : (size_t)EXECVE_MAX_STRLEN);
            if (r < 0) break;
            kargv_ptrs[argc] = strbuf + buf_off;
            buf_off += (size_t)r + 1;
            argc++;
        }
    }
    if (argc == 0) {
        int ci = 0;
        while (ci < EXECVE_MAX_STRLEN - 1 && kpath[ci] && buf_off + 1 < EXECVE_BUF_SIZE) {
            strbuf[buf_off + (size_t)ci] = kpath[ci]; ci++;
        }
        strbuf[buf_off + (size_t)ci] = '\0';
        kargv_ptrs[0] = strbuf + buf_off;
        buf_off += (size_t)ci + 1;
        argc = 1;
    }

    const char *kenvp_ptrs[EXECVE_MAX_ENVS];
    int envc = 0;
    if (envp && (uint64_t)envp < 0x800000000000ULL) {
        for (int i = 0; i < EXECVE_MAX_ENVS; i++) {
            char *const *ep = &envp[i];
            if ((uint64_t)ep + sizeof(char *) > 0x800000000000ULL) break;
            char *env = *ep;
            if (!env) break;
            if ((uint64_t)env >= 0x800000000000ULL) break;
            size_t avail = EXECVE_BUF_SIZE - buf_off;
            if (avail < 2) break;
            int r = copy_path_from_user_proc(strbuf + buf_off, env,
                        avail < (size_t)EXECVE_MAX_STRLEN ? avail : (size_t)EXECVE_MAX_STRLEN);
            if (r < 0) break;
            kenvp_ptrs[envc] = strbuf + buf_off;
            buf_off += (size_t)r + 1;
            envc++;
        }
    }

    /* Identify file source: ext2 inode or ramfs node */
    extern int ext2_inode_read(uint32_t ino, struct ext2_inode *out);
    uint64_t ext2_ino;
    struct vfs_node *ramfs_node;
    size_t elf_len;

    /* Macro to free string buffer on early exit */
    #define EXECVE_FAIL(err) do { pages_free(strbuf, EXECVE_BUF_PAGES); return (err); } while(0)

    int shebang_depth = 0;
    #define SHEBANG_MAX_DEPTH 4

shebang_retry:
    ext2_ino = vfs_ext2_lookup(kpath);
    ramfs_node = 0;
    elf_len = 0;

    if (ext2_ino) {
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)ext2_ino, &ip) < 0 || ip.i_size == 0)
            EXECVE_FAIL(-ENOEXEC);
        elf_len = ip.i_size;
    } else {
        ramfs_node = vfs_lookup(kpath);
        if (!ramfs_node) EXECVE_FAIL(-ENOENT);
        if (ramfs_node->type != VFS_FILE) EXECVE_FAIL(-EACCES);
        if (!ramfs_node->data || ramfs_node->size == 0) EXECVE_FAIL(-ENOEXEC);
        elf_len = ramfs_node->size;
    }

    /* ── Shebang (#!) detection ─────────────────── */
    {
        uint8_t shebang_buf[256];
        size_t peek_len = elf_len < 256 ? elf_len : 256;
        if (ext2_ino) {
            extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
            if (ext2_read((uint32_t)ext2_ino, shebang_buf, 0, peek_len) < (int)peek_len)
                EXECVE_FAIL(-EIO);
        } else {
            kmemcpy(shebang_buf, ramfs_node->data, peek_len);
        }

        if (peek_len >= 2 && shebang_buf[0] == '#' && shebang_buf[1] == '!') {
            if (++shebang_depth > SHEBANG_MAX_DEPTH)
                EXECVE_FAIL(-ELOOP);

            /* Find end of first line */
            int eol = 2;
            while (eol < (int)peek_len && shebang_buf[eol] != '\n') eol++;

            /* Strip trailing \r (Windows line endings) */
            int line_end = eol;
            if (line_end > 2 && shebang_buf[line_end - 1] == '\r') line_end--;

            /* Skip whitespace after #! */
            int pos = 2;
            while (pos < line_end && (shebang_buf[pos] == ' ' || shebang_buf[pos] == '\t')) pos++;

            /* Extract interpreter path */
            int interp_start = pos;
            while (pos < line_end && shebang_buf[pos] != ' ' && shebang_buf[pos] != '\t') pos++;
            int interp_end = pos;

            if (interp_start == interp_end)
                EXECVE_FAIL(-ENOEXEC); /* empty interpreter */

            /* Extract optional single argument */
            while (pos < line_end && (shebang_buf[pos] == ' ' || shebang_buf[pos] == '\t')) pos++;
            int arg_start = pos;
            while (pos < line_end && shebang_buf[pos] != ' ' && shebang_buf[pos] != '\t') pos++;
            int arg_end = pos;
            int has_arg = (arg_start < arg_end);

            /* Save original script path (current kpath) into strbuf */
            char *script_path = strbuf + buf_off;
            int si = 0;
            while (si < PATH_MAX_PROC - 1 && kpath[si]) {
                strbuf[buf_off + (size_t)si] = kpath[si]; si++;
            }
            strbuf[buf_off + (size_t)si] = '\0';
            buf_off += (size_t)si + 1;

            /* Copy interpreter path to kpath */
            int ilen = interp_end - interp_start;
            if (ilen >= PATH_MAX_PROC) EXECVE_FAIL(-ENAMETOOLONG);
            for (int i = 0; i < ilen; i++) kpath[i] = (char)shebang_buf[interp_start + i];
            kpath[ilen] = '\0';

            /* Copy optional arg to strbuf */
            char *shebang_arg = 0;
            if (has_arg) {
                int alen = arg_end - arg_start;
                shebang_arg = strbuf + buf_off;
                for (int i = 0; i < alen; i++)
                    strbuf[buf_off + (size_t)i] = (char)shebang_buf[arg_start + i];
                strbuf[buf_off + (size_t)alen] = '\0';
                buf_off += (size_t)alen + 1;
            }

            /* Build new argv: [interp, opt_arg, script_path, orig_argv[1:]] */
            const char *new_argv[EXECVE_MAX_ARGS];
            int new_argc = 0;

            /* Copy interpreter basename as argv[0] — actually use full path */
            new_argv[new_argc++] = kpath;
            if (has_arg && new_argc < EXECVE_MAX_ARGS)
                new_argv[new_argc++] = shebang_arg;
            if (new_argc < EXECVE_MAX_ARGS)
                new_argv[new_argc++] = script_path;

            /* Append original argv[1:] */
            for (int i = 1; i < argc && new_argc < EXECVE_MAX_ARGS; i++)
                new_argv[new_argc++] = kargv_ptrs[i];

            /* Replace argv */
            argc = new_argc;
            for (int i = 0; i < argc; i++) kargv_ptrs[i] = new_argv[i];

            goto shebang_retry;
        }
    }

    if (elf_len < sizeof(Elf64_Ehdr)) EXECVE_FAIL(-ENOEXEC);

    /* Read only the ELF header to determine type and check for PT_INTERP.
     * elf_load_ext2/elf_load_ramfs will read the full header internally,
     * but we need e_type + interp path before destroying the address space. */
    uint8_t hdr_buf[sizeof(Elf64_Ehdr) + 64 * sizeof(Elf64_Phdr)];
    size_t hdr_read = sizeof(Elf64_Ehdr);
    if (ext2_ino) {
        extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
        if (ext2_read((uint32_t)ext2_ino, hdr_buf, 0, hdr_read) < (int)hdr_read)
            EXECVE_FAIL(-EIO);
    } else {
        kmemcpy(hdr_buf, ramfs_node->data, hdr_read);
    }
    const Elf64_Ehdr *peek_eh = (const Elf64_Ehdr *)hdr_buf;

    /* Read program headers too */
    if (peek_eh->e_phnum > 64) EXECVE_FAIL(-ENOEXEC);
    size_t phdrs_end = (size_t)(peek_eh->e_phoff + (uint64_t)peek_eh->e_phnum * peek_eh->e_phentsize);
    if (phdrs_end > sizeof(hdr_buf) || phdrs_end > elf_len) EXECVE_FAIL(-ENOEXEC);
    if (phdrs_end > hdr_read) {
        size_t extra = phdrs_end - hdr_read;
        if (ext2_ino) {
            extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
            if (ext2_read((uint32_t)ext2_ino, hdr_buf + hdr_read, hdr_read, extra) < (int)extra)
                EXECVE_FAIL(-EIO);
        } else {
            kmemcpy(hdr_buf + hdr_read, ramfs_node->data + hdr_read, extra);
        }
    }

    /* Scan for PT_INTERP */
    int has_interp = 0;
    char interp_path[256];
    interp_path[0] = '\0';
    /* Track interpreter source for streaming load */
    uint64_t interp_ext2_ino = 0;
    struct vfs_node *interp_ramfs = 0;
    size_t interp_len = 0;

    if (peek_eh->e_type == ET_DYN || peek_eh->e_type == ET_EXEC) {
        for (int i = 0; i < peek_eh->e_phnum && i < 64; i++) {
            size_t phoff = (size_t)(peek_eh->e_phoff + (uint64_t)i * peek_eh->e_phentsize);
            if (phoff + sizeof(Elf64_Phdr) > phdrs_end) break;
            const Elf64_Phdr *ph = (const Elf64_Phdr *)(hdr_buf + phoff);
            if (ph->p_type == PT_INTERP) {
                has_interp = 1;
                size_t iplen = ph->p_filesz;
                if (iplen >= sizeof(interp_path)) iplen = sizeof(interp_path) - 1;
                if (ph->p_offset + iplen <= phdrs_end) {
                    kmemcpy(interp_path, hdr_buf + ph->p_offset, iplen);
                } else if (ext2_ino) {
                    extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
                    ext2_read((uint32_t)ext2_ino, interp_path, (size_t)ph->p_offset, iplen);
                } else {
                    kmemcpy(interp_path, ramfs_node->data + ph->p_offset, iplen);
                }
                interp_path[iplen] = '\0';
                while (iplen > 0 && interp_path[iplen - 1] == '\0') iplen--;

                /* Locate interpreter: try ramfs first, then ext2 */
                struct vfs_node *inode = vfs_lookup(interp_path);
                if (inode && inode->type == VFS_FILE && inode->data && inode->size > 0) {
                    interp_ramfs = inode;
                    interp_len = inode->size;
                } else {
                    interp_ext2_ino = vfs_ext2_lookup(interp_path);
                    if (interp_ext2_ino) {
                        struct ext2_inode iip;
                        if (ext2_inode_read((uint32_t)interp_ext2_ino, &iip) == 0)
                            interp_len = iip.i_size;
                    }
                }
                break;
            }
        }
    }

    /* Switch to kernel PML4 before freeing current address space
     * (we're currently running with p->pml4 in CR3) */
    arch_set_cr3(virt_to_phys(pml4));

    /* Free current address space */
    uint64_t exec_irqf;
    spin_lock_irq(&p->lock, &exec_irqf);
    free_address_space(p->pml4);
    vma_free_tree(p->vma_root);
    p->vma_root = 0;
    spin_unlock_irq(&p->lock, exec_irqf);

    /* Create new PML4 */
    p->pml4 = create_user_pml4();
    if (!p->pml4) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        arch_set_cr3(virt_to_phys(pml4));
        thread_return_to_kernel(cur);
        return -ENOMEM;
    }

    /* ASLR */
    uint64_t stack_rand = aslr_rand() & 0xFFF000ULL;
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF via streaming (page-by-page, no large contiguous buffer) */
    uint64_t entry, stack_ptr;
    elf_info_t info;
    int load_rc;

    if (ext2_ino)
        load_rc = elf_load_ext2((uint32_t)ext2_ino, elf_len, p->pml4, 0, &info);
    else
        load_rc = elf_load_ramfs(ramfs_node->data, elf_len, p->pml4, 0, &info);

    if (load_rc < 0) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        arch_set_cr3(virt_to_phys(pml4));
        thread_return_to_kernel(cur);
        return -ENOEXEC;
    }

    /* Create VMAs from header buffer (phdrs already in hdr_buf) */
    spin_lock_irq(&p->lock, &exec_irqf);
    create_elf_vmas(&p->vma_root, hdr_buf, phdrs_end, info.load_base);

    /* Load interpreter if present */
    if (has_interp && (interp_ramfs || interp_ext2_ino) && interp_len > 0) {
        uint64_t interp_base_hint = (info.brk + 0x200000ULL) & ~0xFFFULL;
        elf_info_t interp_info;
        int irc;
        if (interp_ramfs)
            irc = elf_load_ramfs(interp_ramfs->data, interp_len, p->pml4,
                                 interp_base_hint, &interp_info);
        else
            irc = elf_load_ext2((uint32_t)interp_ext2_ino, interp_len, p->pml4,
                                interp_base_hint, &interp_info);
        if (irc < 0) {
            serial_puts("execve: failed to load interpreter\n");
        } else {
            info.interp_base = interp_info.load_base;
            info.entry = interp_info.prog_entry;
            /* Create interp VMAs: read interp header for phdr info */
            /* interp_info already has the metadata; use load_base for VMA offset.
             * We need the interp phdrs — re-read into a small buffer. */
            uint8_t ihdr[sizeof(Elf64_Ehdr) + 64 * sizeof(Elf64_Phdr)];
            size_t ihr = sizeof(Elf64_Ehdr) + 64 * sizeof(Elf64_Phdr);
            if (ihr > interp_len) ihr = interp_len;
            if (interp_ramfs) {
                kmemcpy(ihdr, interp_ramfs->data, ihr);
            } else {
                extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
                ext2_read((uint32_t)interp_ext2_ino, ihdr, 0, ihr);
            }
            create_elf_vmas(&p->vma_root, ihdr, ihr, interp_info.load_base);
        }
    }

    p->brk_base = info.brk;
    p->brk_current = info.brk;
    entry = info.entry;

    uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
    uint64_t guard_bottom = stack_bottom - 4096;
    vma_insert(&p->vma_root, guard_bottom, stack_bottom,
               0 /* PROT_NONE */, MAP_PRIVATE | MAP_ANONYMOUS);
    vma_insert(&p->vma_root, stack_bottom, stack_top,
               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    spin_unlock_irq(&p->lock, exec_irqf);

    stack_ptr = build_user_stack(p->pml4, stack_top,
                                 kargv_ptrs, argc, kenvp_ptrs, envc, &info);
    if (!stack_ptr) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        arch_set_cr3(virt_to_phys(pml4));
        thread_return_to_kernel(cur);
        return -ENOMEM;
    }

    /* Store executable path for /proc/self/exe */
    {
        int ei = 0;
        while (ei < 255 && kpath[ei]) { p->exe_path[ei] = kpath[ei]; ei++; }
        p->exe_path[ei] = '\0';
    }

    /* Store cmdline (null-separated argv) for /proc/pid/cmdline */
    {
        int pos = 0;
        for (int i = 0; i < argc && pos < 1023; i++) {
            int j = 0;
            while (kargv_ptrs[i][j] && pos < 1023) { p->cmdline[pos++] = kargv_ptrs[i][j++]; }
            p->cmdline[pos++] = '\0';
        }
        p->cmdline_len = pos;
    }

    /* Set comm from argv[0] basename */
    {
        const char *s = kargv_ptrs[0];
        const char *base = s;
        for (int i = 0; s[i]; i++) if (s[i] == '/') base = s + i + 1;
        int ci = 0;
        while (ci < 15 && base[ci]) { p->comm[ci] = base[ci]; ci++; }
        p->comm[ci] = '\0';
    }

    /* Free execve string buffer — no longer needed */
    pages_free(strbuf, EXECVE_BUF_PAGES);

    /* vfork: wake blocked parent now that child has its own address space */
    if (p->vfork_parent_tid) {
        thread_t *pt = thread_find_by_tid(p->vfork_parent_tid);
        if (pt) {
            extern void sched_wake(thread_t *t);
            sched_wake(pt);
        }
        p->vfork_parent_tid = 0;
    }

    /* Reset signal dispositions: POSIX requires that after exec, all signals
     * with user handlers are reset to SIG_DFL. SIG_IGN is preserved.
     * Pending signals survive exec — delivered under new disposition. */
    for (int si = 1; si < 64; si++) {
        if ((uint64_t)p->sig_actions[si].sa_handler > 1)
            kmemset(&p->sig_actions[si], 0, sizeof(struct k_sigaction));
    }

    /* Flush event queue — stale events from pre-exec children must not
     * confuse event_wait (e.g., sigtimedwait) after exec replaces the binary. */
    { thread_t *et = cur;
      et->eq.head = 0;
      et->eq.tail = 0;
    }

    /* Close O_CLOEXEC fds — must use full close (pipe_close, fd_cleanup_entry)
     * not just fd_close, so pipe write_open/read_open decrements happen and
     * blocked readers/writers get woken (e.g., posix_spawn error-check pipe). */
    {
        extern void vfs_file_free_obj(void *obj);
        extern void fd_cleanup_entry(int fde_type, void *fde_obj);
        for (int i = 0; i < FD_MAX; i++) {
            if (p->fds.entries[i].type != FD_NONE &&
                (p->fds.entries[i].flags & 0x80000)) { /* O_CLOEXEC */
                int ftype = p->fds.entries[i].type;
                if (ftype == FD_FILE) {
                    vfs_file_free_obj(p->fds.entries[i].obj);
                } else if (ftype != FD_SERIAL) {
                    fd_cleanup_entry(ftype, p->fds.entries[i].obj);
                }
                fd_close(&p->fds, i);
            }
        }
    }

    /* Set up thread for new execution */
    cur->rip = entry;
    cur->rsp = stack_ptr;
    cur->rflags = 0x202;
    cur->rax = 0;
    cur->rbx = 0; cur->rcx = 0; cur->rdx = 0;
    cur->rsi = 0; cur->rdi = 0; cur->rbp = 0;
    cur->r8  = 0; cur->r9  = 0; cur->r10 = 0;
    cur->r11 = 0; cur->r12 = 0; cur->r13 = 0;
    cur->r14 = 0; cur->r15 = 0;
    cur->fs_base = 0;

    /* Reset FPU/SSE/AVX state for new executable.
     * FCW=0x037F: extended precision, all exceptions masked.
     * MXCSR=0x1F80: all SSE exceptions masked, round-to-nearest. */
    kmemset(cur->xsave_area, 0, xsave_size);
    *(uint16_t *)(cur->xsave_area + 0) = 0x037F;  /* FCW */
    *(uint32_t *)(cur->xsave_area + 24) = 0x1F80;  /* MXCSR */
    if (xsave_size > 512)
        *(uint64_t *)(cur->xsave_area + 512) = xsave_xcr0; /* XSTATE_BV */

    /* Load new page tables and jump to userspace */
    arch_set_cr3(virt_to_phys(p->pml4));

    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(cur->kstack_top);
    percpu_self()->kernel_rsp = cur->kstack_top;

    /* Clear FS_BASE — new process has no TLS yet (libc sets it via arch_prctl) */
    arch_set_fs_base(0);

    /* Restore clean FPU state */
    arch_fpstate_restore(cur->xsave_area);

    proc_enter_ring3(cur);
    /* unreachable */
    return 0;
}
