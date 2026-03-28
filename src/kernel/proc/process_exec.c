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
                                 char kargv[][EXECVE_MAX_STRLEN], int argc,
                                 char kenvp[][EXECVE_MAX_STRLEN], int envc,
                                 const elf_info_t *elf_info) {
    /* Map 4 stack pages (16KB) */
    for (int i = 0; i < 4; i++) {
        uint64_t va = stack_top - (uint64_t)(i + 1) * 4096;
        uint64_t *pg = alloc_page();
        if (!pg) return 0;
        if (map_user_page(user_pml4, va, virt_to_phys(pg), PROT_READ | PROT_WRITE) < 0)
            return 0;
    }

    /* Allocate the stack-top page we can write to (overwrites previous mapping) */
    uint64_t stk_page_va = stack_top - 4096;
    uint64_t *frame_page = alloc_page();
    if (!frame_page) return 0;
    map_user_page(user_pml4, stk_page_va, virt_to_phys(frame_page), PROT_READ | PROT_WRITE);
    uint8_t *page = (uint8_t *)frame_page;

    /* Write strings at the top of the page */
    uint64_t str_off = 4096;
    uint64_t argv_addrs[EXECVE_MAX_ARGS];
    uint64_t envp_addrs[EXECVE_MAX_ENVS];

    /* 16 random bytes for AT_RANDOM */
    str_off -= 16;
    str_off &= ~7ULL;
    uint64_t at_random_addr = stk_page_va + str_off;
    extern int random_get(void *buf, size_t len);
    if (random_get(page + str_off, 16) != 0) {
        /* Fallback: zero-fill (better than nothing) */
        kmemset(page + str_off, 0x42, 16);
    }

    /* Environment strings — with bounds checking to prevent underflow */
    for (int i = envc - 1; i >= 0; i--) {
        int sl = 0; while (kenvp[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) return 0; /* not enough space */
        str_off -= (uint64_t)(sl + 1);
        kmemcpy(page + str_off, kenvp[i], (size_t)(sl + 1));
        envp_addrs[i] = stk_page_va + str_off;
    }
    /* Argument strings — with bounds checking to prevent underflow */
    for (int i = argc - 1; i >= 0; i--) {
        int sl = 0; while (kargv[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) return 0; /* not enough space */
        str_off -= (uint64_t)(sl + 1);
        kmemcpy(page + str_off, kargv[i], (size_t)(sl + 1));
        argv_addrs[i] = stk_page_va + str_off;
    }

    str_off &= ~7ULL;

    /* Count qwords: argc(1) + argv(argc+1) + envp(envc+1) + auxv(8*2+2) */
    int naux = 8; /* PHDR, PHENT, PHNUM, BASE, ENTRY, PAGESZ, RANDOM, NULL */
    int nqwords = 1 + (argc + 1) + (envc + 1) + (naux * 2);
    str_off -= (uint64_t)nqwords * 8;
    str_off &= ~0xFULL; /* 16-byte align RSP at process entry */

    uint64_t *stk = (uint64_t *)(page + str_off);
    uint64_t *sp_base = stk;

    /* argc */
    *stk++ = (uint64_t)argc;
    /* argv pointers */
    for (int i = 0; i < argc; i++)
        *stk++ = argv_addrs[i];
    *stk++ = 0; /* argv terminator */
    /* envp pointers */
    for (int i = 0; i < envc; i++)
        *stk++ = envp_addrs[i];
    *stk++ = 0; /* envp terminator */
    /* auxv */
    *stk++ = AT_PHDR;   *stk++ = elf_info->prog_phdr;
    *stk++ = AT_PHENT;  *stk++ = (uint64_t)elf_info->prog_phent;
    *stk++ = AT_PHNUM;  *stk++ = (uint64_t)elf_info->prog_phnum;
    *stk++ = AT_BASE;   *stk++ = elf_info->interp_base;
    *stk++ = AT_ENTRY;  *stk++ = elf_info->prog_entry;
    *stk++ = AT_PAGESZ; *stk++ = 4096;
    *stk++ = AT_RANDOM; *stk++ = at_random_addr;
    *stk++ = AT_NULL;   *stk++ = 0;

    uint64_t sp = stk_page_va + (uint64_t)((uint8_t *)sp_base - page);
    return sp;
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

    /* Copy path to kernel buffer before using it */
    char kpath[PATH_MAX_PROC];
    int plen = copy_path_from_user_proc(kpath, path, PATH_MAX_PROC);
    if (plen < 0) return -EFAULT;

    /* Copy argv/envp from userspace before destroying address space */
    char kargv[EXECVE_MAX_ARGS][EXECVE_MAX_STRLEN];
    int argc = 0;
    if (argv && (uint64_t)argv < 0x800000000000ULL) {
        for (int i = 0; i < EXECVE_MAX_ARGS; i++) {
            char *const *ap = &argv[i];
            if ((uint64_t)ap + sizeof(char *) > 0x800000000000ULL) break;
            char *arg = *ap;
            if (!arg) break;
            if ((uint64_t)arg >= 0x800000000000ULL) break;
            int r = copy_path_from_user_proc(kargv[argc], arg, EXECVE_MAX_STRLEN);
            if (r < 0) break;
            argc++;
        }
    }
    if (argc == 0) {
        int ci = 0;
        while (ci < EXECVE_MAX_STRLEN - 1 && kpath[ci]) { kargv[0][ci] = kpath[ci]; ci++; }
        kargv[0][ci] = '\0';
        argc = 1;
    }

    char kenvp[EXECVE_MAX_ENVS][EXECVE_MAX_STRLEN];
    int envc = 0;
    if (envp && (uint64_t)envp < 0x800000000000ULL) {
        for (int i = 0; i < EXECVE_MAX_ENVS; i++) {
            char *const *ep = &envp[i];
            if ((uint64_t)ep + sizeof(char *) > 0x800000000000ULL) break;
            char *env = *ep;
            if (!env) break;
            if ((uint64_t)env >= 0x800000000000ULL) break;
            int r = copy_path_from_user_proc(kenvp[envc], env, EXECVE_MAX_STRLEN);
            if (r < 0) break;
            envc++;
        }
    }

    /* Load binary into buffer — ramfs or ext2, one path */
    uint8_t *elf_buf = 0;
    size_t elf_len = 0;
    int elf_pages = 0;

    uint64_t ext2_ino = vfs_ext2_lookup(kpath);
    if (ext2_ino) {
        /* ext2: read entire file into buffer */
        extern int ext2_inode_read(uint32_t ino, struct ext2_inode *out);
        extern int ext2_read(uint32_t ino, void *buf, size_t offset, size_t len);
        struct ext2_inode ip;
        if (ext2_inode_read((uint32_t)ext2_ino, &ip) < 0 || ip.i_size == 0) return -ENOEXEC;
        elf_len = ip.i_size;
        elf_pages = (int)((elf_len + 4095) / 4096);
        elf_buf = (uint8_t *)pages_alloc(elf_pages);
        if (!elf_buf) return -ENOMEM;
        int rd = ext2_read((uint32_t)ext2_ino, elf_buf, 0, elf_len);
        if (rd < (int)elf_len) { pages_free(elf_buf, elf_pages); return -EIO; }
    } else {
        /* ramfs: copy from node */
        struct vfs_node *node = vfs_lookup(kpath);
        if (!node) return -ENOENT;
        if (node->type != VFS_FILE) return -EACCES;
        if (!node->data || node->size == 0) return -ENOEXEC;
        elf_len = node->size;
        elf_pages = (int)((elf_len + 4095) / 4096);
        elf_buf = (uint8_t *)pages_alloc(elf_pages);
        if (!elf_buf) return -ENOMEM;
        kmemcpy(elf_buf, node->data, elf_len);
    }

    /* ELF header from buffer */
    if (elf_len < sizeof(Elf64_Ehdr)) {
        pages_free(elf_buf, elf_pages);
        return -ENOEXEC;
    }
    const Elf64_Ehdr *peek_eh = (const Elf64_Ehdr *)elf_buf;

    /* Determine if dynamic and check for PT_INTERP */
    int has_interp = 0;
    uint8_t *interp_buf = 0;
    size_t interp_len = 0;
    int interp_pages = 0;

    if (peek_eh->e_type == ET_DYN || peek_eh->e_type == ET_EXEC) {
        /* Scan phdrs for PT_INTERP */
        for (int i = 0; i < peek_eh->e_phnum && i < 64; i++) {
            size_t phoff = (size_t)(peek_eh->e_phoff + (uint64_t)i * peek_eh->e_phentsize);
            if (phoff + sizeof(Elf64_Phdr) > elf_len) break;
            Elf64_Phdr ph;
            kmemcpy(&ph, elf_buf + phoff, sizeof(ph));
            if (ph.p_type == PT_INTERP) {
                has_interp = 1;
                char ipath[256];
                size_t iplen = ph.p_filesz;
                if (iplen >= sizeof(ipath)) iplen = sizeof(ipath) - 1;
                if (ph.p_offset + iplen <= elf_len)
                    kmemcpy(ipath, elf_buf + ph.p_offset, iplen);
                ipath[iplen] = '\0';
                while (iplen > 0 && ipath[iplen - 1] == '\0') iplen--;

                int irc = vfs_read_file(ipath, &interp_buf, &interp_len);
                if (irc == 0)
                    interp_pages = (int)((interp_len + 4095) / 4096);
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
        if (elf_buf) pages_free(elf_buf, elf_pages);
        if (interp_buf) pages_free(interp_buf, interp_pages);
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

    uint64_t entry, stack_ptr;
    int use_ex = (peek_eh->e_type == ET_DYN || has_interp);

    if (use_ex) {
        /* Extended path: ET_DYN and/or PT_INTERP */
        elf_info_t info;
        int load_rc;
        load_rc = elf_load_ex(elf_buf, elf_len, p->pml4, 0, &info);

        if (load_rc < 0) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            arch_set_cr3(virt_to_phys(pml4));
            thread_return_to_kernel(cur);
            return -ENOEXEC;
        }

        /* Create VMAs (skip for CosmoFS — segments already mapped) */
        spin_lock_irq(&p->lock, &exec_irqf);
        create_elf_vmas(&p->vma_root, elf_buf, elf_len, info.load_base);

        /* Load interpreter if present */
        if (has_interp && interp_buf) {
            uint64_t interp_base_hint = (info.brk + 0x200000ULL) & ~0xFFFULL;
            elf_info_t interp_info;
            if (elf_load_ex(interp_buf, interp_len, p->pml4,
                            interp_base_hint, &interp_info) < 0) {
                serial_puts("execve: failed to load interpreter\n");
            } else {
                info.interp_base = interp_info.load_base;
                info.entry = interp_info.prog_entry;
                create_elf_vmas(&p->vma_root, interp_buf, interp_len,
                                interp_info.load_base);
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
        /* brk VMA created on first brk() call, not here (avoid zero-length VMA) */

        stack_ptr = build_user_stack(p->pml4, stack_top,
                                     kargv, argc, kenvp, envc, &info);
        if (!stack_ptr) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            arch_set_cr3(virt_to_phys(pml4));
            thread_return_to_kernel(cur);
            return -ENOMEM;
        }
    } else {
        /* Fast path: plain ET_EXEC, no interpreter — use elf_load_ex + build_user_stack */
        elf_info_t info;
        int load_rc;
        load_rc = elf_load_ex(elf_buf, elf_len, p->pml4, 0, &info);

        if (load_rc < 0) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            arch_set_cr3(virt_to_phys(pml4));
            thread_return_to_kernel(cur);
            return -ENOEXEC;
        }

        p->brk_base = info.brk;
        p->brk_current = info.brk;
        entry = info.entry;

        /* Stack VMA with guard page */
        spin_lock_irq(&p->lock, &exec_irqf);
        uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
        uint64_t guard_bottom = stack_bottom - 4096;
        vma_insert(&p->vma_root, guard_bottom, stack_bottom,
                   0 /* PROT_NONE */, MAP_PRIVATE | MAP_ANONYMOUS);
        vma_insert(&p->vma_root, stack_bottom, stack_top,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

        /* Create VMAs for ELF segments */
        create_elf_vmas(&p->vma_root, elf_buf, elf_len, 0);
        spin_unlock_irq(&p->lock, exec_irqf);

        stack_ptr = build_user_stack(p->pml4, stack_top,
                                      kargv, argc, kenvp, envc, &info);
        if (!stack_ptr) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            arch_set_cr3(virt_to_phys(pml4));
            thread_return_to_kernel(cur);
            return -ENOMEM;
        }
    }

    if (elf_buf) pages_free(elf_buf, elf_pages);
    if (interp_buf) pages_free(interp_buf, interp_pages);

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
            while (kargv[i][j] && pos < 1023) { p->cmdline[pos++] = kargv[i][j++]; }
            p->cmdline[pos++] = '\0';
        }
        p->cmdline_len = pos;
    }

    /* Set comm from argv[0] basename */
    {
        const char *s = kargv[0];
        const char *base = s;
        for (int i = 0; s[i]; i++) if (s[i] == '/') base = s + i + 1;
        int ci = 0;
        while (ci < 15 && base[ci]) { p->comm[ci] = base[ci]; ci++; }
        p->comm[ci] = '\0';
    }

    /* Reset signal dispositions: POSIX requires that after exec, all signals
     * with user handlers are reset to SIG_DFL. SIG_IGN is preserved.
     * Pending signals survive exec — delivered under new disposition. */
    for (int si = 1; si < 64; si++) {
        if ((uint64_t)p->sig_actions[si].sa_handler > 1)
            kmemset(&p->sig_actions[si], 0, sizeof(struct k_sigaction));
    }

    /* Close O_CLOEXEC fds */
    for (int i = 0; i < FD_MAX; i++) {
        if (p->fds.entries[i].type != FD_NONE &&
            (p->fds.entries[i].flags & 0x80000)) { /* O_CLOEXEC */
            fd_close(&p->fds, i);
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

    /* Reset FPU/SSE state for new executable */
    kmemset(cur->fxsave_area, 0, 512);
    *(uint32_t *)(cur->fxsave_area + 24) = 0x1F80; /* MXCSR: all exceptions masked */

    /* Load new page tables and jump to userspace */
    arch_set_cr3(virt_to_phys(p->pml4));

    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(cur->kstack_top);
    percpu_self()->kernel_rsp = cur->kstack_top;

    /* Clear FS_BASE — new process has no TLS yet (libc sets it via arch_prctl) */
    arch_set_fs_base(0);

    /* Restore clean FPU state */
    arch_fxrstor(cur->fxsave_area);

    proc_enter_ring3(cur);
    /* unreachable */
    return 0;
}
