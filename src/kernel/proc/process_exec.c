/* CosmoRT Process - execve */

#include "proc/proc_internal.h"
#include "linux/capability.h"
#include "sys/vdso.h"

/* ── execve helpers ──────────────────────────────── */

#define PATH_MAX_PROC 4096

/* execve needs plen >= 0 for an empty argv/envp string; copy_path_from_user
 * returns -ENOENT on i==0. Wrap it so callers see length 0 for empty. */
static int copy_path_from_user_proc(char *kbuf, const char *upath, size_t max) {
    int n = copy_path_from_user(kbuf, upath, max);
    if (n == -ENOENT) { kbuf[0] = '\0'; return 0; }
    return n;
}

/* STK_PAGES × 4KB = user-stack scratch area mapped for argv/envp/auxv setup.
 * 8 pages (32KB) reicht fuer typische gcc/ld Invocations. */
#define STK_PAGES 8
#define STK_SIZE  ((uint64_t)STK_PAGES * 4096)

/* Pointer-Arrays heap-alloziert: bei EXECVE_MAX_ARGS=4096 waere jedes
 * Array 32KB und wuerde den 16KB-Kernel-Stack sprengen. */
#define EXECVE_PTR_PAGES \
    ((EXECVE_MAX_ARGS * (int)sizeof(uint64_t) + 4095) / 4096)

_Static_assert(EXECVE_MAX_ARGS >= 4096, "ARG_MAX-Kompat braucht >=4096 Slots");
_Static_assert(EXECVE_MAX_ENVS >= 4096, "ARG_MAX-Kompat braucht >=4096 Slots");
_Static_assert(EXECVE_MAX_STRLEN >= 128 * 1024, "Linux MAX_ARG_STRLEN = 128KB");
_Static_assert(EXECVE_BUF_PAGES <= 512, "buddy cap: pages_alloc max 512");
_Static_assert(EXECVE_PTR_PAGES <= 512, "buddy cap: pages_alloc max 512");
_Static_assert(EXECVE_MAX_ARGS == EXECVE_MAX_ENVS, "PTR-Array shared size");

/* Build user stack with argv, envp, auxv. Allocates stack pages.
 * Returns RSP value on success, 0 on error. */

uint64_t build_user_stack(uint64_t *user_pml4, uint64_t stack_top,
                                 const char *const *argv, int argc,
                                 const char *const *envp, int envc,
                                 const elf_info_t *elf_info) {
    uint8_t *stk_kern[STK_PAGES];
    uint64_t stk_base_va = stack_top - STK_SIZE;
    for (int i = 0; i < STK_PAGES; i++) {
        uint64_t *pg = alloc_page();
        if (!pg) return 0;
        uint64_t va = stk_base_va + (uint64_t)i * 4096;
        if (map_user_page(user_pml4, va, virt_to_phys(pg), PROT_READ | PROT_WRITE) < 0)
            return 0;
        stk_kern[i] = (uint8_t *)pg;
    }

    #define STK_BYTE(off) (stk_kern[(off) >> 12][(off) & 0xFFF])

    uint64_t *argv_addrs = (uint64_t *)pages_alloc(EXECVE_PTR_PAGES);
    uint64_t *envp_addrs = argv_addrs ? (uint64_t *)pages_alloc(EXECVE_PTR_PAGES) : 0;
    if (!argv_addrs || !envp_addrs) {
        if (argv_addrs) pages_free(argv_addrs, EXECVE_PTR_PAGES);
        if (envp_addrs) pages_free(envp_addrs, EXECVE_PTR_PAGES);
        return 0;
    }

    uint64_t str_off = STK_SIZE;

    str_off -= 16;
    str_off &= ~7ULL;
    uint64_t at_random_addr = stk_base_va + str_off;
    extern int random_get(void *buf, size_t len);
    {
        uint8_t rnd[16];
        if (random_get(rnd, 16) != 0)
            kmemset(rnd, 0x42, 16);
        for (int i = 0; i < 16; i++)
            STK_BYTE(str_off + (uint64_t)i) = rnd[i];
    }

    uint64_t ret = 0;

    for (int i = envc - 1; i >= 0; i--) {
        int sl = 0; while (envp[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) goto out;
        str_off -= (uint64_t)(sl + 1);
        for (int j = 0; j <= sl; j++)
            STK_BYTE(str_off + (uint64_t)j) = (uint8_t)envp[i][j];
        envp_addrs[i] = stk_base_va + str_off;
    }
    for (int i = argc - 1; i >= 0; i--) {
        int sl = 0; while (argv[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) goto out;
        str_off -= (uint64_t)(sl + 1);
        for (int j = 0; j <= sl; j++)
            STK_BYTE(str_off + (uint64_t)j) = (uint8_t)argv[i][j];
        argv_addrs[i] = stk_base_va + str_off;
    }

    str_off &= ~7ULL;

    /* argc(1) + argv(argc+1) + envp(envc+1) + auxv(naux*2+2 incl. AT_NULL).
     * AT_SYSINFO_EHDR only emitted if the new pml4 actually has the vDSO
     * mapped (vdso_map skips non-init time_namespaces). */
    uint64_t vdso_base = vdso_user_base_for(user_pml4);
    int naux = vdso_base ? 9 : 8;
    int nqwords = 1 + (argc + 1) + (envc + 1) + (naux * 2);
    str_off -= (uint64_t)nqwords * 8;
    str_off &= ~0xFULL; /* 16-byte align RSP am Entry */

    uint64_t wp = str_off;
    #define STK_QWORD(off, val) do { \
        uint64_t _v = (val); \
        for (int _b = 0; _b < 8; _b++) \
            STK_BYTE((off) + (uint64_t)_b) = (uint8_t)(_v >> (_b * 8)); \
    } while(0)

    STK_QWORD(wp, (uint64_t)argc); wp += 8;
    for (int i = 0; i < argc; i++) { STK_QWORD(wp, argv_addrs[i]); wp += 8; }
    STK_QWORD(wp, 0); wp += 8;
    for (int i = 0; i < envc; i++) { STK_QWORD(wp, envp_addrs[i]); wp += 8; }
    STK_QWORD(wp, 0); wp += 8;
    STK_QWORD(wp, AT_PHDR);          wp += 8; STK_QWORD(wp, elf_info->prog_phdr);          wp += 8;
    STK_QWORD(wp, AT_PHENT);         wp += 8; STK_QWORD(wp, (uint64_t)elf_info->prog_phent); wp += 8;
    STK_QWORD(wp, AT_PHNUM);         wp += 8; STK_QWORD(wp, (uint64_t)elf_info->prog_phnum); wp += 8;
    STK_QWORD(wp, AT_BASE);          wp += 8; STK_QWORD(wp, elf_info->interp_base);         wp += 8;
    STK_QWORD(wp, AT_ENTRY);         wp += 8; STK_QWORD(wp, elf_info->prog_entry);          wp += 8;
    STK_QWORD(wp, AT_PAGESZ);        wp += 8; STK_QWORD(wp, 4096);                          wp += 8;
    STK_QWORD(wp, AT_RANDOM);        wp += 8; STK_QWORD(wp, at_random_addr);                wp += 8;
    if (vdso_base) {
        STK_QWORD(wp, AT_SYSINFO_EHDR); wp += 8; STK_QWORD(wp, vdso_base);                  wp += 8;
    }
    STK_QWORD(wp, AT_NULL);          wp += 8; STK_QWORD(wp, 0);                             wp += 8;

    #undef STK_QWORD

    ret = stk_base_va + str_off;
out:
    pages_free(argv_addrs, EXECVE_PTR_PAGES);
    pages_free(envp_addrs, EXECVE_PTR_PAGES);
    #undef STK_BYTE
    return ret;
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

/* Innere Variante: kpath ist bereits ein kernel-string, vollstaendig
 * resolved. argv/envp bleiben User-Pointer. Fuer do_execveat, das den
 * Pfad selber zusammenbaut (dirfd + relpath -> combined). */
static long do_execve_kpath(char kpath[PATH_MAX_PROC],
                            char *const argv[], char *const envp[]);

long do_execve(const char *path, char *const argv[], char *const envp[]) {
    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;

    char kpath_raw[PATH_MAX_PROC], kpath[PATH_MAX_PROC];
    int plen = copy_path_from_user_proc(kpath_raw, path, PATH_MAX_PROC);
    if (plen < 0) return -EFAULT;
    extern int resolve_path(const char *path, char *out, int outsize);
    resolve_path(kpath_raw, kpath, PATH_MAX_PROC);

    return do_execve_kpath(kpath, argv, envp);
}

static long do_execve_kpath(char kpath[PATH_MAX_PROC],
                            char *const argv[], char *const envp[]) {
    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *p = cur->proc;

    /* String-Buffer + Pointer-Arrays heap-alloziert: Bei Linux-kompatiblen
     * Limits (EXECVE_MAX_ARGS=4096) waeren Stack-Arrays je 32KB und wuerden
     * den Kernel-Stack sprengen. Jeweils pro execve-Call. */
    char *strbuf = (char *)pages_alloc(EXECVE_BUF_PAGES);
    if (!strbuf) return -ENOMEM;
    const char **kargv_ptrs = (const char **)pages_alloc(EXECVE_PTR_PAGES);
    const char **kenvp_ptrs = kargv_ptrs ? (const char **)pages_alloc(EXECVE_PTR_PAGES) : 0;
    if (!kargv_ptrs || !kenvp_ptrs) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        if (kargv_ptrs) pages_free(kargv_ptrs, EXECVE_PTR_PAGES);
        if (kenvp_ptrs) pages_free(kenvp_ptrs, EXECVE_PTR_PAGES);
        return -ENOMEM;
    }

    #define EXECVE_EARLY_FAIL(err) do { \
        pages_free(strbuf, EXECVE_BUF_PAGES); \
        pages_free(kargv_ptrs, EXECVE_PTR_PAGES); \
        pages_free(kenvp_ptrs, EXECVE_PTR_PAGES); \
        return (err); \
    } while(0)

    size_t buf_off = 0;

    int argc = 0;
    if (argv && (uint64_t)argv < 0x800000000000ULL) {
        for (int i = 0; ; i++) {
            if (i >= EXECVE_MAX_ARGS) EXECVE_EARLY_FAIL(-E2BIG);
            char *const *ap = &argv[i];
            if ((uint64_t)ap + sizeof(char *) > 0x800000000000ULL) EXECVE_EARLY_FAIL(-EFAULT);
            char *arg = *ap;
            if (!arg) break;
            if ((uint64_t)arg >= 0x800000000000ULL) EXECVE_EARLY_FAIL(-EFAULT);
            size_t avail = EXECVE_BUF_SIZE - buf_off;
            if (avail < 2) EXECVE_EARLY_FAIL(-E2BIG);
            size_t cap = avail < (size_t)EXECVE_MAX_STRLEN ? avail : (size_t)EXECVE_MAX_STRLEN;
            int r = copy_path_from_user_proc(strbuf + buf_off, arg, cap);
            if (r == -ENAMETOOLONG) {
                /* Linux: zu langer String ODER Gesamt-Buffer voll → -E2BIG. */
                EXECVE_EARLY_FAIL(-E2BIG);
            }
            if (r < 0) EXECVE_EARLY_FAIL(r);
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

    int envc = 0;
    if (envp && (uint64_t)envp < 0x800000000000ULL) {
        for (int i = 0; ; i++) {
            if (i >= EXECVE_MAX_ENVS) EXECVE_EARLY_FAIL(-E2BIG);
            char *const *ep = &envp[i];
            if ((uint64_t)ep + sizeof(char *) > 0x800000000000ULL) EXECVE_EARLY_FAIL(-EFAULT);
            char *env = *ep;
            if (!env) break;
            if ((uint64_t)env >= 0x800000000000ULL) EXECVE_EARLY_FAIL(-EFAULT);
            size_t avail = EXECVE_BUF_SIZE - buf_off;
            if (avail < 2) EXECVE_EARLY_FAIL(-E2BIG);
            size_t cap = avail < (size_t)EXECVE_MAX_STRLEN ? avail : (size_t)EXECVE_MAX_STRLEN;
            int r = copy_path_from_user_proc(strbuf + buf_off, env, cap);
            if (r == -ENAMETOOLONG) EXECVE_EARLY_FAIL(-E2BIG);
            if (r < 0) EXECVE_EARLY_FAIL(r);
            kenvp_ptrs[envc] = strbuf + buf_off;
            buf_off += (size_t)r + 1;
            envc++;
        }
    }

    /* Identify file source: ext4 inode or ramfs inode */
    extern int ext4_inode_read(uint32_t ino, struct ext4_inode *out);
    uint64_t ext4_ino;
    struct vfs_inode *ramfs_node;
    size_t elf_len;

    #define EXECVE_FAIL(err) do { \
        pages_free(strbuf, EXECVE_BUF_PAGES); \
        pages_free(kargv_ptrs, EXECVE_PTR_PAGES); \
        pages_free(kenvp_ptrs, EXECVE_PTR_PAGES); \
        return (err); \
    } while(0)

    int shebang_depth = 0;
    #define SHEBANG_MAX_DEPTH 4

shebang_retry:;
    /* Validate path first — propagate ENAMETOOLONG, ENOTDIR, ELOOP etc. */
    struct k_stat exec_st;
    { int st_rc = vfs_stat(kpath, &exec_st);
      if (st_rc < 0) EXECVE_FAIL(st_rc); }
    if ((exec_st.st_mode & S_IFMT) == S_IFDIR) EXECVE_FAIL(-EACCES);
    if (!(exec_st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) EXECVE_FAIL(-EACCES);
    /* DAC: Linux bprm_execve erzwingt Execute-Permission nach euid/egid/groups.
     * Root (euid==0) darf alles ausser total-nicht-exec (schon oben gefangen).
     * Non-root muss die passende x-Bit (user/group/other) tragen. */
    {
        int rc = cred_may_access(p, exec_st.st_uid, exec_st.st_gid,
                                 exec_st.st_mode, MAY_EXEC);
        if (rc < 0) EXECVE_FAIL(rc);
    }

    ext4_ino = vfs_ext4_lookup(kpath);
    ramfs_node = 0;
    elf_len = 0;

    if (ext4_ino) {
        struct ext4_inode ip;
        if (ext4_inode_read((uint32_t)ext4_ino, &ip) < 0 || ip.i_size == 0)
            EXECVE_FAIL(-ENOEXEC);
        elf_len = ip.i_size;
    } else {
        struct vfs_node *dentry = vfs_lookup(kpath);
        if (!dentry || !dentry->inode) EXECVE_FAIL(-ENOENT);
        ramfs_node = dentry->inode;
        if (ramfs_node->type != VFS_FILE) EXECVE_FAIL(-EACCES);
        if (!ramfs_node->data || ramfs_node->size == 0) EXECVE_FAIL(-ENOEXEC);
        elf_len = ramfs_node->size;
    }

    /* ETXTBSY gate: Linux fs/exec.c denies write before format validation.
     * Must run before ELF-header/shebang checks so a writer-held binary
     * fails with -ETXTBSY rather than -ENOEXEC. Only check against the
     * initial inode (pre-shebang); interpreters go through recursive
     * execve paths which re-enter and re-check. */
    if (shebang_depth == 0) {
        int new_backend = ext4_ino ? VFS_BACKEND_EXT4 : VFS_BACKEND_RAM;
        uint64_t new_ino = ext4_ino ? ext4_ino : ramfs_node->ino;
        if (wc_peek_writers(new_backend, new_ino) > 0)
            EXECVE_FAIL(-ETXTBSY);
    }


    /* ── Shebang (#!) detection ─────────────────── */
    {
        uint8_t shebang_buf[256];
        size_t peek_len = elf_len < 256 ? elf_len : 256;
        if (ext4_ino) {
            extern int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
            if (ext4_read((uint32_t)ext4_ino, shebang_buf, 0, peek_len) < (int)peek_len)
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

            /* new_argv: [interp, opt_arg?, script_path, orig_argv[1:]] — heap,
             * weil das Array bei EXECVE_MAX_ARGS=4096 den Kernel-Stack sprengt. */
            const char **new_argv = (const char **)pages_alloc(EXECVE_PTR_PAGES);
            if (!new_argv) EXECVE_FAIL(-ENOMEM);
            int new_argc = 0;

            new_argv[new_argc++] = kpath;
            if (has_arg) {
                if (new_argc >= EXECVE_MAX_ARGS) {
                    pages_free(new_argv, EXECVE_PTR_PAGES);
                    EXECVE_FAIL(-E2BIG);
                }
                new_argv[new_argc++] = shebang_arg;
            }
            if (new_argc >= EXECVE_MAX_ARGS) {
                pages_free(new_argv, EXECVE_PTR_PAGES);
                EXECVE_FAIL(-E2BIG);
            }
            new_argv[new_argc++] = script_path;

            for (int i = 1; i < argc; i++) {
                if (new_argc >= EXECVE_MAX_ARGS) {
                    pages_free(new_argv, EXECVE_PTR_PAGES);
                    EXECVE_FAIL(-E2BIG);
                }
                new_argv[new_argc++] = kargv_ptrs[i];
            }

            argc = new_argc;
            for (int i = 0; i < argc; i++) kargv_ptrs[i] = new_argv[i];
            pages_free(new_argv, EXECVE_PTR_PAGES);

            goto shebang_retry;
        }
    }

    if (elf_len < sizeof(Elf64_Ehdr)) EXECVE_FAIL(-ENOEXEC);

    /* Read only the ELF header to determine type and check for PT_INTERP.
     * elf_load_ext4/elf_load_ramfs will read the full header internally,
     * but we need e_type + interp path before destroying the address space. */
    uint8_t hdr_buf[sizeof(Elf64_Ehdr) + 64 * sizeof(Elf64_Phdr)];
    size_t hdr_read = sizeof(Elf64_Ehdr);
    if (ext4_ino) {
        extern int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
        if (ext4_read((uint32_t)ext4_ino, hdr_buf, 0, hdr_read) < (int)hdr_read)
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
        if (ext4_ino) {
            extern int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
            if (ext4_read((uint32_t)ext4_ino, hdr_buf + hdr_read, hdr_read, extra) < (int)extra)
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
    uint64_t interp_ext4_ino = 0;
    struct vfs_inode *interp_ramfs = 0;
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
                } else if (ext4_ino) {
                    extern int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
                    ext4_read((uint32_t)ext4_ino, interp_path, (size_t)ph->p_offset, iplen);
                } else {
                    kmemcpy(interp_path, ramfs_node->data + ph->p_offset, iplen);
                }
                interp_path[iplen] = '\0';
                while (iplen > 0 && interp_path[iplen - 1] == '\0') iplen--;

                /* Locate interpreter: try ramfs first, then ext4 */
                struct vfs_node *interp_dentry = vfs_lookup(interp_path);
                struct vfs_inode *interp_ino = interp_dentry ? interp_dentry->inode : 0;
                if (interp_ino && interp_ino->type == VFS_FILE && interp_ino->data && interp_ino->size > 0) {
                    interp_ramfs = interp_ino;
                    interp_len = interp_ino->size;
                } else {
                    interp_ext4_ino = vfs_ext4_lookup(interp_path);
                    if (interp_ext4_ino) {
                        struct ext4_inode iip;
                        if (ext4_inode_read((uint32_t)interp_ext4_ino, &iip) == 0)
                            interp_len = iip.i_size;
                    }
                }
                break;
            }
        }
    }

    /* ETXTBSY: reject exec if the target inode has writable opens.
     * Linux fs/exec.c: deny_write_access before destroying old mm. This is
     * the last failure point before the point-of-no-return MMU switch.
     * Same-inode re-exec of own text: allow (already denied). */
    {
        int new_backend = ext4_ino ? VFS_BACKEND_EXT4 : VFS_BACKEND_RAM;
        uint64_t new_ino = ext4_ino ? ext4_ino : ramfs_node->ino;
        if (!(p->exe_backend == new_backend && p->exe_ino == new_ino)) {
            int rc = i_writecount_deny_write(new_backend, new_ino);
            if (rc < 0) EXECVE_FAIL(rc);
            if (p->exe_ino)
                i_writecount_allow_write(p->exe_backend, p->exe_ino);
            p->exe_backend = new_backend;
            p->exe_ino = new_ino;
        }
    }

    /* Switch to kernel PML4 before freeing current address space
     * (we're currently running with p->pml4 in CR3) */
    hal_mmu_switch(virt_to_phys(pml4));

    /* Free current address space.
     * mm_shared (CLONE_VM child doing execve, e.g. vfork pattern): parent
     * still owns the pml4/vmas, so don't free them — just detach. */
    uint64_t exec_irqf;
    spin_lock_irq(&p->lock, &exec_irqf);
    if (!p->mm_shared) {
        free_address_space(p->pml4);
        vma_free_tree(p->vma_root);
    }
    p->mm_shared = 0;
    p->vma_root = 0;
    spin_unlock_irq(&p->lock, exec_irqf);

    /* Create new PML4 */
    p->pml4 = create_user_pml4();
    if (!p->pml4) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        pages_free(kargv_ptrs, EXECVE_PTR_PAGES);
        pages_free(kenvp_ptrs, EXECVE_PTR_PAGES);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        hal_mmu_switch(virt_to_phys(pml4));
        schedule();
        __builtin_unreachable();
    }

    /* vDSO: shared kernel-owned pages. Mapped read-only into every user-mm
     * so __vdso_clock_gettime can run without a syscall. The AT_SYSINFO_EHDR
     * auxv entry below tells musl where to find them. VMA registration
     * keeps mm_mmap from allocating over the canonical address. */
    vdso_map(p->pml4, &p->vma_root);

    /* ASLR */
    uint64_t stack_rand = aslr_rand() & 0x3FFFFF000ULL; /* 22-bit, 4KB aligned */
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF via streaming (page-by-page, no large contiguous buffer) */
    uint64_t entry, stack_ptr;
    elf_info_t info;
    int load_rc;

    if (ext4_ino)
        load_rc = elf_load_ext4((uint32_t)ext4_ino, elf_len, p->pml4, 0, &info);
    else
        load_rc = elf_load_ramfs(ramfs_node->data, elf_len, p->pml4, 0, &info);

    if (load_rc < 0) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        pages_free(kargv_ptrs, EXECVE_PTR_PAGES);
        pages_free(kenvp_ptrs, EXECVE_PTR_PAGES);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        hal_mmu_switch(virt_to_phys(pml4));
        schedule();
        __builtin_unreachable();
    }

    /* Create VMAs from header buffer (phdrs already in hdr_buf) */
    spin_lock_irq(&p->lock, &exec_irqf);
    create_elf_vmas(&p->vma_root, hdr_buf, phdrs_end, info.load_base);

    /* Load interpreter if present */
    if (has_interp && (interp_ramfs || interp_ext4_ino) && interp_len > 0) {
        uint64_t interp_base_hint = (info.brk + 0x200000ULL) & ~0xFFFULL;
        elf_info_t interp_info;
        int irc;
        if (interp_ramfs)
            irc = elf_load_ramfs(interp_ramfs->data, interp_len, p->pml4,
                                 interp_base_hint, &interp_info);
        else
            irc = elf_load_ext4((uint32_t)interp_ext4_ino, interp_len, p->pml4,
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
                extern int ext4_read(uint32_t ino, void *buf, size_t offset, size_t len);
                ext4_read((uint32_t)interp_ext4_ino, ihdr, 0, ihr);
            }
            create_elf_vmas(&p->vma_root, ihdr, ihr, interp_info.load_base);
        }
    }

    p->brk_base = info.brk;
    p->brk_current = info.brk;
    entry = info.entry;

    /* Stack: small initial VMA with VMA_GROWSDOWN, plus PROT_NONE guard page at
     * the maximum-growth limit to turn stack overflow / stack-clash into
     * deterministic SIGSEGV. Linux: `stack_guard_gap` + VM_GROWSDOWN. */
    uint64_t stack_bottom = stack_top - USER_STACK_INIT;
    uint64_t stack_floor  = stack_top - RLIM_STACK_DEFAULT;
    vma_insert(&p->vma_root, stack_bottom, stack_top,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | VMA_GROWSDOWN);
    vma_insert(&p->vma_root, stack_floor - STACK_GUARD_SIZE, stack_floor,
               0, MAP_PRIVATE | MAP_ANONYMOUS);
    p->stack_top = stack_top;
    spin_unlock_irq(&p->lock, exec_irqf);

    stack_ptr = build_user_stack(p->pml4, stack_top,
                                 kargv_ptrs, argc, kenvp_ptrs, envc, &info);
    if (!stack_ptr) {
        pages_free(strbuf, EXECVE_BUF_PAGES);
        pages_free(kargv_ptrs, EXECVE_PTR_PAGES);
        pages_free(kenvp_ptrs, EXECVE_PTR_PAGES);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        hal_mmu_switch(virt_to_phys(pml4));
        schedule();
        __builtin_unreachable();
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

    /* Exec-Scratch nicht mehr benoetigt — User-Stack hat alles kopiert. */
    pages_free(strbuf, EXECVE_BUF_PAGES);
    pages_free(kargv_ptrs, EXECVE_PTR_PAGES);
    pages_free(kenvp_ptrs, EXECVE_PTR_PAGES);

    /* vfork: wake blocked parent now that child has its own address space */
    if (p->vfork_parent_tid) {
        thread_t *pt = thread_find_by_tid(p->vfork_parent_tid);
        if (pt) {
            extern void sched_wake(thread_t *t);
            sched_wake(pt);
        }
        p->vfork_parent_tid = 0;
    }

    /* OOM-Tuning bei credential change (SUID/SGID exec) zuruecksetzen.
     * Linux begin_new_exec(): bprm->secureexec → security_bprm_committed_creds
     *   → set_dumpable + Reset von oom_score_adj{,_min} auf 0 wenn aktuelle
     *   euid != originale ruid (SUID-Wechsel) und kein CAP_SYS_RESOURCE.
     * Wir haben (noch) keinen SUID-Pfad; defensiv: wenn euid waehrend des
     * exec-Pfads geaendert wurde (zukuenftige Erweiterung), reset hier. */
    {
        uint32_t st_uid = exec_st.st_uid;
        uint32_t st_gid = exec_st.st_gid;
        int suid_set = (exec_st.st_mode & S_ISUID) && st_uid != p->ruid;
        int sgid_set = (exec_st.st_mode & S_ISGID) && st_gid != p->rgid &&
                       (exec_st.st_mode & S_IXGRP);
        if ((suid_set || sgid_set) &&
            !(p->cap_effective & CAP_TO_MASK(CAP_SYS_RESOURCE))) {
            p->oom_score_adj     = 0;
            p->oom_score_adj_min = 0;
        }
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
    event_queue_reset(&cur->eq);

    /* Close O_CLOEXEC fds — must use full close (pipe_close, fd_cleanup_entry)
     * not just fd_close, so pipe write_open/read_open decrements happen and
     * blocked readers/writers get woken (e.g., posix_spawn error-check pipe). */
    {
        extern void vfs_file_free_obj(void *obj);
        extern void fd_cleanup_entry(int fde_type, void *fde_obj, int fde_flags);
        for (int i = 0; i < p->fds.max_slots; i++) {
            fd_entry_t *e = fd_entry_at(&p->fds, i);
            if (!e || e->type == FD_NONE) continue;
            if (!(e->flags & 0x80000)) continue;  /* O_CLOEXEC */
            int ftype = e->type;
            if (ftype == FD_FILE) {
                vfs_file_free_obj(e->obj);
            } else if (ftype != FD_SERIAL) {
                fd_cleanup_entry(ftype, e->obj, e->flags);
            }
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

    /* Reset FPU/SSE/AVX state for new executable.
     * FCW=0x037F: extended precision, all exceptions masked.
     * MXCSR=0x1F80: all SSE exceptions masked, round-to-nearest. */
    kmemset(cur->xsave_area, 0, xsave_size);
    *(uint16_t *)(cur->xsave_area + 0) = 0x037F;  /* FCW */
    *(uint32_t *)(cur->xsave_area + 24) = 0x1F80;  /* MXCSR */
    if (xsave_size > 512)
        *(uint64_t *)(cur->xsave_area + 512) = xsave_xcr0; /* XSTATE_BV */

    /* Load new page tables and jump to userspace */
    hal_mmu_switch(virt_to_phys(p->pml4));

    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(cur->kstack_top);
    percpu_self()->kernel_rsp = cur->kstack_top;

    /* Clear TLS - new process has no TLS yet (libc sets it via arch_prctl) */
    hal_cpu_set_tls(0);

    /* Restore clean FPU state */
    hal_cpu_fpu_restore(cur->xsave_area);

    proc_enter_ring3(cur);
    /* unreachable */
    return 0;
}

/* ── execveat(2) ─────────────────────────────────── */

long do_execveat(int dirfd, const char *path, char *const argv[],
                 char *const envp[], int flags) {
    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) return -EINVAL;

    char kpath[PATH_MAX_PROC];
    int plen = copy_path_from_user_proc(kpath, path, PATH_MAX_PROC);
    if (plen < 0) return plen;

    extern int resolve_path(const char *p, char *out, int outsize);
    char rkpath[PATH_MAX_PROC];

    if (plen == 0) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        thread_t *cur = thread_current();
        if (!cur || !cur->proc) return -EFAULT;
        process_t *p = cur->proc;
        if (dirfd == AT_FDCWD) return -ENOENT;
        fd_entry_t *fde = fd_get(&p->fds, dirfd);
        if (!fde || fde->type == FD_NONE) return -EBADF;
        if (fde->type != FD_FILE) return -EACCES;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (!f || !f->path[0]) return -EBADF;
        if (f->type == VFS_DIR) return -EACCES;
        resolve_path(f->path, rkpath, PATH_MAX_PROC);
        return do_execve_kpath(rkpath, argv, envp);
    }

    if (kpath[0] != '/') {
        if (dirfd == AT_FDCWD) {
            resolve_path(kpath, rkpath, PATH_MAX_PROC);
            return do_execve_kpath(rkpath, argv, envp);
        }
        thread_t *cur = thread_current();
        if (!cur || !cur->proc) return -EFAULT;
        process_t *p = cur->proc;
        fd_entry_t *fde = fd_get(&p->fds, dirfd);
        if (!fde || fde->type == FD_NONE) return -EBADF;
        if (fde->type != FD_FILE) return -ENOTDIR;
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (!f || f->type != VFS_DIR) return -ENOTDIR;
        if (!f->path[0]) return -EBADF;
        char combined[PATH_MAX_PROC];
        int di = 0;
        const char *dp = f->path;
        while (*dp && di < PATH_MAX_PROC - 2) combined[di++] = *dp++;
        if (di > 0 && combined[di - 1] != '/') combined[di++] = '/';
        const char *rp = kpath;
        while (*rp && di < PATH_MAX_PROC - 1) combined[di++] = *rp++;
        combined[di] = '\0';
        if (flags & AT_SYMLINK_NOFOLLOW) {
            extern struct vfs_node *vfs_lookup_nofollow(const char *path, int *err);
            int err = 0;
            struct vfs_node *n = vfs_lookup_nofollow(combined, &err);
            if (n && n->inode && n->inode->type == VFS_SYMLINK) return -ELOOP;
        }
        resolve_path(combined, rkpath, PATH_MAX_PROC);
        return do_execve_kpath(rkpath, argv, envp);
    }

    if (flags & AT_SYMLINK_NOFOLLOW) {
        extern struct vfs_node *vfs_lookup_nofollow(const char *p, int *err);
        int err = 0;
        struct vfs_node *n = vfs_lookup_nofollow(kpath, &err);
        if (n && n->inode && n->inode->type == VFS_SYMLINK) return -ELOOP;
    }
    resolve_path(kpath, rkpath, PATH_MAX_PROC);
    return do_execve_kpath(rkpath, argv, envp);
}
