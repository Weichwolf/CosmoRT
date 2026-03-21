/* CosmoRT Dynamic Linker (ld-cosmo.so)
 *
 * Minimal userspace ELF interpreter. Freestanding, no libc.
 * Parses auxv, applies R_X86_64_RELATIVE relocations for PIE,
 * resolves GLOB_DAT/JUMP_SLOT/64 via DT_HASH, loads one level
 * of DT_NEEDED, provides dlopen/dlsym/dlclose/dlerror, jumps
 * to program entry.
 */

/* ── Types (freestanding, no stdint.h) ──────────── */

typedef unsigned long      uint64_t;
typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;
typedef long               int64_t;
typedef int                int32_t;
typedef unsigned long      size_t;
typedef long               ssize_t;

#define NULL ((void *)0)

/* ── Syscall wrappers ───────────────────────────── */

static long sc1(long n, long a) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10")=d;
    register long r8 __asm__("r8")=e;
    register long r9 __asm__("r9")=f;
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
    return r;
}

#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_mmap        9
#define SYS_mprotect    10
#define SYS_munmap      11
#define SYS_exit_group  231

#define O_RDONLY  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* ── Output helpers ─────────────────────────────── */

static void puts(const char *s) {
    int n = 0;
    while (s[n]) n++;
    sc3(SYS_write, 1, (long)s, n);
}

static void put_hex(uint64_t v) {
    char buf[17];
    int i = 0;
    if (v == 0) { puts("0"); return; }
    while (v) { buf[i++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    char out[17];
    int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

__attribute__((noreturn))
static void die(const char *msg) {
    puts("ld-cosmo: FATAL: ");
    puts(msg);
    puts("\n");
    sc1(SYS_exit_group, 127);
    __builtin_unreachable();
}

/* ── String helpers ─────────────────────────────── */

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
}

/* ── ELF structures ─────────────────────────────── */

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ET_DYN  3

#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_INIT      12
#define DT_FINI      13
#define DT_PLTREL    20
#define DT_JMPREL    23
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xffffffff))
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define STB_GLOBAL 1
#define STB_WEAK   2
#define ELF64_ST_BIND(i) ((i) >> 4)

#define SHN_UNDEF 0

/* Auxiliary vector types */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9

/* ── dlopen constants ──────────────────────────── */

#define RTLD_LAZY    1
#define RTLD_NOW     2
#define RTLD_DEFAULT ((void *)(uint64_t)-1)

/* ── dl_handle: loaded shared object ─────────────── */

#define DLOPEN_MAX 16

typedef struct {
    uint64_t    base;       /* relocation base (map_addr - align_min) */
    uint64_t    map_addr;   /* mmap return value (for munmap) */
    uint64_t    map_size;   /* total mapped size (for munmap) */
    Elf64_Sym  *symtab;     /* DT_SYMTAB (mapped) */
    const char *strtab;     /* DT_STRTAB (mapped) */
    uint32_t   *hashtab;    /* DT_HASH (mapped) */
    uint64_t    init;       /* DT_INIT address (0 if none) */
    uint64_t    fini;       /* DT_FINI address (0 if none) */
    uint64_t    init_array; /* DT_INIT_ARRAY address (0 if none) */
    uint64_t    init_arraysz;
    uint64_t    fini_array; /* DT_FINI_ARRAY address (0 if none) */
    uint64_t    fini_arraysz;
    int         in_use;
    int         refcount;
} dl_handle_t;

static dl_handle_t dl_handles[DLOPEN_MAX];
static char dl_errbuf[128];
static int  dl_err_set;

static void dl_set_error(const char *msg) {
    size_t n = strlen(msg);
    if (n >= sizeof(dl_errbuf)) n = sizeof(dl_errbuf) - 1;
    memcpy(dl_errbuf, msg, n);
    dl_errbuf[n] = '\0';
    dl_err_set = 1;
}

/* ── ELF hash ───────────────────────────────────── */

static uint32_t elf_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* ── Library descriptor ─────────────────────────── */

typedef struct {
    uint64_t    base;
    Elf64_Sym  *symtab;
    const char *strtab;
    uint32_t   *hash;      /* DT_HASH: [nbucket, nchain, buckets..., chains...] */
} lib_t;

/* Main program lib_t (set during _start, used by dlsym RTLD_DEFAULT) */
static lib_t dl_main_lib;
static int   dl_main_lib_valid;

/* All DT_NEEDED libs for RTLD_DEFAULT search */
static lib_t dl_needed_libs[8];
static int   dl_needed_libs_count;

/* Look up a symbol by name in a library using DT_HASH.
 * Returns symbol value (base-adjusted) or 0 if not found. */
static uint64_t lib_lookup(const lib_t *lib, const char *name) {
    if (!lib->hash || !lib->symtab || !lib->strtab)
        return 0;

    uint32_t nbucket = lib->hash[0];
    /* uint32_t nchain = lib->hash[1]; */
    const uint32_t *buckets = &lib->hash[2];
    const uint32_t *chains  = &lib->hash[2 + nbucket];

    uint32_t h = elf_hash(name) % nbucket;
    for (uint32_t idx = buckets[h]; idx != 0; idx = chains[idx]) {
        const Elf64_Sym *sym = &lib->symtab[idx];
        if (sym->st_shndx == SHN_UNDEF) continue;
        uint8_t bind = ELF64_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (streq(name, lib->strtab + sym->st_name))
            return lib->base + sym->st_value;
    }
    return 0;
}

/* ── Parse .dynamic and fill lib_t ──────────────── */

static void parse_dynamic(Elf64_Dyn *dyn, uint64_t base, lib_t *out,
                          const char **needed, int *needed_count, int max_needed) {
    out->base = base;
    out->symtab = NULL;
    out->strtab = NULL;
    out->hash = NULL;

    /* First pass: find strtab (needed for DT_NEEDED strings) */
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB) {
            out->strtab = (const char *)(base + d->d_val);
            break;
        }
    }

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   out->symtab = (Elf64_Sym *)(base + d->d_val); break;
        case DT_HASH:     out->hash = (uint32_t *)(base + d->d_val); break;
        case DT_NEEDED:
            if (out->strtab && *needed_count < max_needed)
                needed[(*needed_count)++] = out->strtab + d->d_val;
            break;
        default: break;
        }
    }
}

/* ── Apply relocations ──────────────────────────── */

static void apply_rela(Elf64_Rela *rela, uint64_t count, uint64_t base,
                       const Elf64_Sym *symtab, const char *strtab,
                       const lib_t *libs, int lib_count) {
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint64_t *target = (uint64_t *)(base + rela[i].r_offset);

        switch (type) {
        case R_X86_64_RELATIVE:
            *target = base + (uint64_t)rela[i].r_addend;
            break;

        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_64: {
            uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
            if (!symtab || !strtab) {
                puts("ld-cosmo: reloc without symtab\n");
                break;
            }
            const Elf64_Sym *sym = &symtab[sym_idx];
            const char *name = strtab + sym->st_name;

            /* Try program's own symbols first */
            uint64_t val = 0;
            if (sym->st_shndx != SHN_UNDEF)
                val = base + sym->st_value;

            /* Then search loaded libraries */
            if (!val) {
                for (int l = 0; l < lib_count; l++) {
                    val = lib_lookup(&libs[l], name);
                    if (val) break;
                }
            }

            if (!val) {
                puts("ld-cosmo: unresolved: ");
                puts(name);
                puts("\n");
                /* Leave as zero — will crash on use, which is better than
                 * aborting the whole load for optional symbols. */
            }

            if (type == R_X86_64_64)
                *target = val + (uint64_t)rela[i].r_addend;
            else
                *target = val;
            break;
        }

        case R_X86_64_NONE:
            break;

        default:
            puts("ld-cosmo: unknown reloc type ");
            put_hex(type);
            puts("\n");
            break;
        }
    }
}

/* ── Load a shared library from VFS ─────────────── */

static int load_library(const char *name, lib_t *out) {
    /* Build path: /lib/<name> */
    char path[128] = "/lib/";
    size_t plen = 5;
    size_t nlen = strlen(name);
    if (plen + nlen >= sizeof(path)) return -1;
    memcpy(path + plen, name, nlen + 1);

    puts("ld-cosmo: loading ");
    puts(path);
    puts("\n");

    long fd = sc3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        puts("ld-cosmo: open failed: ");
        puts(path);
        puts("\n");
        return -1;
    }

    /* Read ELF header */
    Elf64_Ehdr ehdr;
    long n = sc3(SYS_read, fd, (long)&ehdr, (long)sizeof(ehdr));
    if (n < (long)sizeof(ehdr)) {
        sc1(SYS_close, fd);
        die("short read on library ELF header");
    }

    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        sc1(SYS_close, fd);
        die("bad ELF magic in library");
    }

    /* Calculate total mapping extent from program headers */
    uint64_t min_vaddr = (uint64_t)-1, max_vaddr = 0;
    Elf64_Phdr phdr;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        /* Seek by reading up to the right offset — simple, no lseek needed
         * since we're reading headers sequentially after ehdr.
         * Actually we need lseek or re-read. Use mmap-based approach instead:
         * read all phdrs into a buffer. */
        (void)phdr; (void)i;
        break;
    }

    /* Simpler approach: read the entire file into anonymous memory,
     * then parse and map segments from there. This works for small libs. */

    /* First, figure out file size by reading in chunks */
    uint8_t *filebuf;
    size_t filesize = 0;
    size_t bufcap = 256 * 1024; /* 256KB initial */
    long buf_addr = sc6(SYS_mmap, 0, (long)bufcap, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf_addr < 0) {
        sc1(SYS_close, fd);
        return -1;
    }
    filebuf = (uint8_t *)buf_addr;

    /* Copy header already read */
    memcpy(filebuf, &ehdr, sizeof(ehdr));
    filesize = sizeof(ehdr);

    /* Read rest */
    for (;;) {
        if (filesize >= bufcap) break; /* safety */
        n = sc3(SYS_read, fd, (long)(filebuf + filesize), (long)(bufcap - filesize));
        if (n <= 0) break;
        filesize += (size_t)n;
    }
    sc1(SYS_close, fd);

    /* Parse phdrs from filebuf */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)filebuf;
    min_vaddr = (uint64_t)-1;
    max_vaddr = 0;

    Elf64_Dyn *dyn_file = NULL;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD) {
            if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
            uint64_t end = ph->p_vaddr + ph->p_memsz;
            if (end > max_vaddr) max_vaddr = end;
        }
        if (ph->p_type == PT_DYNAMIC)
            dyn_file = (Elf64_Dyn *)(filebuf + ph->p_offset);
    }

    if (min_vaddr == (uint64_t)-1 || max_vaddr == 0) {
        sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);
        return -1;
    }

    /* Align addresses to page */
    uint64_t align_min = min_vaddr & ~0xFFFULL;
    uint64_t total_size = ((max_vaddr + 0xFFF) & ~0xFFFULL) - align_min;

    /* Reserve address range */
    long map_base = sc6(SYS_mmap, 0, (long)total_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_base < 0) {
        sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);
        return -1;
    }

    uint64_t base = (uint64_t)map_base - align_min;

    /* Map segments: copy from filebuf, zero BSS */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        uint64_t vaddr = base + ph->p_vaddr;
        if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= filesize)
            memcpy((void *)vaddr, filebuf + ph->p_offset, (size_t)ph->p_filesz);
        /* BSS: zero from end of filesz to memsz */
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(vaddr + ph->p_filesz), 0, (size_t)(ph->p_memsz - ph->p_filesz));
    }

    /* Set segment permissions */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        int prot = 0;
        if (ph->p_flags & PF_R) prot |= PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;

        uint64_t seg_start = (base + ph->p_vaddr) & ~0xFFFULL;
        uint64_t seg_end = (base + ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        sc3(SYS_mprotect, (long)seg_start, (long)(seg_end - seg_start), prot);
    }

    /* Parse .dynamic (which is now in our mapping) */
    Elf64_Dyn *dyn_mapped = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) {
            dyn_mapped = (Elf64_Dyn *)(base + ph->p_vaddr);
            break;
        }
    }

    (void)dyn_file; /* used only as fallback, dyn_mapped is the relocated one */

    /* Free file buffer */
    sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);

    if (dyn_mapped) {
        const char *lib_needed[4];
        int lib_needed_count = 0;
        parse_dynamic(dyn_mapped, base, out, lib_needed, &lib_needed_count, 4);

        /* Apply library's own RELATIVE relocations */
        Elf64_Rela *lib_rela = NULL;
        uint64_t lib_relasz = 0;
        Elf64_Rela *lib_jmprel = NULL;
        uint64_t lib_pltrelsz = 0;
        for (Elf64_Dyn *d = dyn_mapped; d->d_tag != DT_NULL; d++) {
            switch (d->d_tag) {
            case DT_RELA:     lib_rela = (Elf64_Rela *)(base + d->d_val); break;
            case DT_RELASZ:   lib_relasz = d->d_val; break;
            case DT_JMPREL:   lib_jmprel = (Elf64_Rela *)(base + d->d_val); break;
            case DT_PLTRELSZ: lib_pltrelsz = d->d_val; break;
            default: break;
            }
        }
        if (lib_rela && lib_relasz)
            apply_rela(lib_rela, lib_relasz / sizeof(Elf64_Rela), base,
                       out->symtab, out->strtab, NULL, 0);
        if (lib_jmprel && lib_pltrelsz)
            apply_rela(lib_jmprel, lib_pltrelsz / sizeof(Elf64_Rela), base,
                       out->symtab, out->strtab, NULL, 0);
    } else {
        out->base = base;
        out->symtab = NULL;
        out->strtab = NULL;
        out->hash = NULL;
    }

    puts("ld-cosmo: loaded at base=");
    put_hex(base);
    puts("\n");

    return 0;
}

/* ── dlopen/dlsym/dlclose/dlerror ────────────────── */

typedef void (*init_fn_t)(void);

void *dlopen(const char *filename, int flags) {
    (void)flags; /* RTLD_LAZY treated as RTLD_NOW — no lazy binding */

    /* NULL filename: return pseudo-handle to main executable */
    if (!filename) {
        if (!dl_main_lib_valid) {
            dl_set_error("dlopen: main program not linked dynamically");
            return NULL;
        }
        /* Use sentinel address as "main program" handle */
        return (void *)1;
    }

    /* Find a free slot */
    dl_handle_t *h = NULL;
    int slot = -1;
    for (int i = 0; i < DLOPEN_MAX; i++) {
        if (!dl_handles[i].in_use) { h = &dl_handles[i]; slot = i; break; }
    }
    if (!h) {
        dl_set_error("dlopen: too many open libraries");
        return NULL;
    }

    /* Build path: if filename contains '/', use as-is; else prepend /lib/ */
    char path[256];
    int has_slash = 0;
    for (const char *p = filename; *p; p++) {
        if (*p == '/') { has_slash = 1; break; }
    }
    if (has_slash) {
        size_t n = strlen(filename);
        if (n >= sizeof(path)) {
            dl_set_error("dlopen: path too long");
            return NULL;
        }
        memcpy(path, filename, n + 1);
    } else {
        size_t plen = 5; /* "/lib/" */
        size_t nlen = strlen(filename);
        if (plen + nlen >= sizeof(path)) {
            dl_set_error("dlopen: path too long");
            return NULL;
        }
        memcpy(path, "/lib/", 5);
        memcpy(path + 5, filename, nlen + 1);
    }

    /* Open file */
    long fd = sc3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        dl_set_error("dlopen: cannot open library");
        return NULL;
    }

    /* Read entire file into anonymous memory */
    size_t bufcap = 256 * 1024;
    long buf_addr = sc6(SYS_mmap, 0, (long)bufcap, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf_addr < 0) {
        sc1(SYS_close, fd);
        dl_set_error("dlopen: mmap failed for read buffer");
        return NULL;
    }
    uint8_t *filebuf = (uint8_t *)buf_addr;
    size_t filesize = 0;

    for (;;) {
        if (filesize >= bufcap) break;
        long n = sc3(SYS_read, fd, (long)(filebuf + filesize), (long)(bufcap - filesize));
        if (n <= 0) break;
        filesize += (size_t)n;
    }
    sc1(SYS_close, fd);

    /* Validate ELF */
    if (filesize < sizeof(Elf64_Ehdr)) {
        sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);
        dl_set_error("dlopen: file too small for ELF header");
        return NULL;
    }
    Elf64_Ehdr *eh = (Elf64_Ehdr *)filebuf;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);
        dl_set_error("dlopen: bad ELF magic");
        return NULL;
    }

    /* Compute total mapping extent */
    uint64_t min_vaddr = (uint64_t)-1, max_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD) {
            if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
            uint64_t end = ph->p_vaddr + ph->p_memsz;
            if (end > max_vaddr) max_vaddr = end;
        }
    }

    if (min_vaddr == (uint64_t)-1 || max_vaddr == 0) {
        sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);
        dl_set_error("dlopen: no PT_LOAD segments");
        return NULL;
    }

    uint64_t align_min = min_vaddr & ~0xFFFULL;
    uint64_t total_size = ((max_vaddr + 0xFFF) & ~0xFFFULL) - align_min;

    /* Reserve address range */
    long map_base = sc6(SYS_mmap, 0, (long)total_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_base < 0) {
        sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);
        dl_set_error("dlopen: mmap failed for library segments");
        return NULL;
    }
    uint64_t base = (uint64_t)map_base - align_min;

    /* Copy PT_LOAD segments, zero BSS */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        uint64_t vaddr = base + ph->p_vaddr;
        if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= filesize)
            memcpy((void *)vaddr, filebuf + ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(vaddr + ph->p_filesz), 0, (size_t)(ph->p_memsz - ph->p_filesz));
    }

    /* Set segment permissions */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        int prot = 0;
        if (ph->p_flags & PF_R) prot |= PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;
        uint64_t seg_start = (base + ph->p_vaddr) & ~0xFFFULL;
        uint64_t seg_end = (base + ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        sc3(SYS_mprotect, (long)seg_start, (long)(seg_end - seg_start), prot);
    }

    /* Find PT_DYNAMIC in mapped memory */
    Elf64_Dyn *dyn_mapped = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > filesize)
            break;
        Elf64_Phdr *ph = (Elf64_Phdr *)(filebuf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) {
            dyn_mapped = (Elf64_Dyn *)(base + ph->p_vaddr);
            break;
        }
    }

    /* Done with file buffer */
    sc6(SYS_munmap, buf_addr, (long)bufcap, 0, 0, 0, 0);

    /* Populate handle */
    h->base = base;
    h->map_addr = (uint64_t)map_base;
    h->map_size = total_size;
    h->symtab = NULL;
    h->strtab = NULL;
    h->hashtab = NULL;
    h->init = 0;
    h->fini = 0;
    h->init_array = 0;
    h->init_arraysz = 0;
    h->fini_array = 0;
    h->fini_arraysz = 0;
    h->in_use = 1;
    h->refcount = 1;

    if (dyn_mapped) {
        /* Parse dynamic section */
        Elf64_Rela *rela = NULL, *jmprel = NULL;
        uint64_t relasz = 0, pltrelsz = 0;

        for (Elf64_Dyn *d = dyn_mapped; d->d_tag != DT_NULL; d++) {
            switch (d->d_tag) {
            case DT_SYMTAB:       h->symtab = (Elf64_Sym *)(base + d->d_val); break;
            case DT_STRTAB:       h->strtab = (const char *)(base + d->d_val); break;
            case DT_HASH:         h->hashtab = (uint32_t *)(base + d->d_val); break;
            case DT_RELA:         rela = (Elf64_Rela *)(base + d->d_val); break;
            case DT_RELASZ:       relasz = d->d_val; break;
            case DT_JMPREL:       jmprel = (Elf64_Rela *)(base + d->d_val); break;
            case DT_PLTRELSZ:     pltrelsz = d->d_val; break;
            case DT_INIT:         h->init = base + d->d_val; break;
            case DT_FINI:         h->fini = base + d->d_val; break;
            case DT_INIT_ARRAY:   h->init_array = base + d->d_val; break;
            case DT_INIT_ARRAYSZ: h->init_arraysz = d->d_val; break;
            case DT_FINI_ARRAY:   h->fini_array = base + d->d_val; break;
            case DT_FINI_ARRAYSZ: h->fini_arraysz = d->d_val; break;
            default: break;
            }
        }

        /* Apply relocations. Search main program + all loaded libs for symbols. */
        lib_t self_lib = { base, h->symtab, h->strtab, h->hashtab };
        lib_t search_libs[DLOPEN_MAX + 2];
        int search_count = 0;

        /* Self first */
        search_libs[search_count++] = self_lib;
        /* Main program */
        if (dl_main_lib_valid)
            search_libs[search_count++] = dl_main_lib;
        /* Other loaded libs */
        for (int i = 0; i < dl_needed_libs_count && search_count < DLOPEN_MAX + 2; i++)
            search_libs[search_count++] = dl_needed_libs[i];

        if (rela && relasz)
            apply_rela(rela, relasz / sizeof(Elf64_Rela), base,
                       h->symtab, h->strtab, search_libs, search_count);
        if (jmprel && pltrelsz)
            apply_rela(jmprel, pltrelsz / sizeof(Elf64_Rela), base,
                       h->symtab, h->strtab, search_libs, search_count);
    }

    /* Run DT_INIT */
    if (h->init) {
        init_fn_t fn = (init_fn_t)h->init;
        fn();
    }

    /* Run DT_INIT_ARRAY */
    if (h->init_array && h->init_arraysz) {
        uint64_t count = h->init_arraysz / sizeof(uint64_t);
        uint64_t *arr = (uint64_t *)h->init_array;
        for (uint64_t i = 0; i < count; i++) {
            if (arr[i]) {
                init_fn_t fn = (init_fn_t)arr[i];
                fn();
            }
        }
    }

    /* Return opaque handle: pointer offset by 2 to distinguish from NULL and (void*)1 */
    return (void *)(uint64_t)(slot + 2);
}

void *dlsym(void *handle, const char *symbol) {
    if (!symbol) {
        dl_set_error("dlsym: NULL symbol name");
        return NULL;
    }

    /* RTLD_DEFAULT: search all loaded objects */
    if (handle == RTLD_DEFAULT || handle == NULL) {
        /* Search main program first */
        if (dl_main_lib_valid) {
            uint64_t val = lib_lookup(&dl_main_lib, symbol);
            if (val) return (void *)val;
        }
        /* Search all DT_NEEDED libs */
        for (int i = 0; i < dl_needed_libs_count; i++) {
            uint64_t val = lib_lookup(&dl_needed_libs[i], symbol);
            if (val) return (void *)val;
        }
        /* Search dlopen'd handles */
        for (int i = 0; i < DLOPEN_MAX; i++) {
            if (!dl_handles[i].in_use) continue;
            lib_t lib = { dl_handles[i].base, dl_handles[i].symtab,
                          dl_handles[i].strtab, dl_handles[i].hashtab };
            uint64_t val = lib_lookup(&lib, symbol);
            if (val) return (void *)val;
        }
        dl_set_error("dlsym: symbol not found");
        return NULL;
    }

    /* handle == (void*)1: main program */
    if (handle == (void *)1) {
        if (!dl_main_lib_valid) {
            dl_set_error("dlsym: main program has no dynamic symbols");
            return NULL;
        }
        uint64_t val = lib_lookup(&dl_main_lib, symbol);
        if (val) return (void *)val;
        dl_set_error("dlsym: symbol not found in main program");
        return NULL;
    }

    /* Normal handle: slot index + 2 */
    int slot = (int)((uint64_t)handle - 2);
    if (slot < 0 || slot >= DLOPEN_MAX || !dl_handles[slot].in_use) {
        dl_set_error("dlsym: invalid handle");
        return NULL;
    }

    dl_handle_t *h = &dl_handles[slot];
    lib_t lib = { h->base, h->symtab, h->strtab, h->hashtab };
    uint64_t val = lib_lookup(&lib, symbol);
    if (val) return (void *)val;

    dl_set_error("dlsym: symbol not found");
    return NULL;
}

int dlclose(void *handle) {
    if (!handle || handle == (void *)1 || handle == RTLD_DEFAULT) {
        dl_set_error("dlclose: invalid handle");
        return -1;
    }

    int slot = (int)((uint64_t)handle - 2);
    if (slot < 0 || slot >= DLOPEN_MAX || !dl_handles[slot].in_use) {
        dl_set_error("dlclose: invalid handle");
        return -1;
    }

    dl_handle_t *h = &dl_handles[slot];
    h->refcount--;
    if (h->refcount > 0) return 0;

    /* Run DT_FINI_ARRAY (in reverse order) */
    if (h->fini_array && h->fini_arraysz) {
        uint64_t count = h->fini_arraysz / sizeof(uint64_t);
        uint64_t *arr = (uint64_t *)h->fini_array;
        for (uint64_t i = count; i > 0; i--) {
            if (arr[i - 1]) {
                init_fn_t fn = (init_fn_t)arr[i - 1];
                fn();
            }
        }
    }

    /* Run DT_FINI */
    if (h->fini) {
        init_fn_t fn = (init_fn_t)h->fini;
        fn();
    }

    /* Unmap the mapped region */
    sc6(SYS_munmap, (long)h->map_addr, (long)h->map_size, 0, 0, 0, 0);

    h->in_use = 0;
    h->symtab = NULL;
    h->strtab = NULL;
    h->hashtab = NULL;
    h->base = 0;
    h->map_addr = 0;
    h->map_size = 0;

    return 0;
}

char *dlerror(void) {
    if (!dl_err_set) return NULL;
    dl_err_set = 0;
    return dl_errbuf;
}

/* ── Entry point ────────────────────────────────── */

void _start(void) {
    /* RSP points to: [argc][argv...][NULL][envp...][NULL][auxv...] */
    register uint64_t *sp __asm__("rsp");

    /* Read argc */
    uint64_t argc = sp[0];

    /* Skip argv (argc entries + NULL terminator) */
    uint64_t *p = sp + 1 + argc + 1;

    /* Skip envp (walk to NULL) */
    while (*p) p++;
    p++; /* past the NULL */

    /* Parse auxv */
    uint64_t at_phdr = 0, at_phnum = 0, at_phent = 0;
    uint64_t at_entry = 0, at_base = 0, at_pagesz = 0;

    for (; p[0] != AT_NULL; p += 2) {
        switch (p[0]) {
        case AT_PHDR:   at_phdr   = p[1]; break;
        case AT_PHENT:  at_phent  = p[1]; break;
        case AT_PHNUM:  at_phnum  = p[1]; break;
        case AT_PAGESZ: at_pagesz = p[1]; break;
        case AT_BASE:   at_base   = p[1]; break;
        case AT_ENTRY:  at_entry  = p[1]; break;
        default: break;
        }
    }

    puts("ld-cosmo: dynamic linker started\n");
    puts("  AT_PHDR=");   put_hex(at_phdr);
    puts(" AT_PHNUM=");   put_hex(at_phnum);
    puts(" AT_PHENT=");   put_hex(at_phent);   puts("\n");
    puts("  AT_ENTRY=");  put_hex(at_entry);
    puts(" AT_BASE=");    put_hex(at_base);
    puts(" AT_PAGESZ=");  put_hex(at_pagesz);  puts("\n");

    (void)at_pagesz;

    if (!at_phdr || !at_phnum || !at_entry) {
        /* No program to link — we were loaded directly. */
        puts("ld-cosmo: no program (AT_ENTRY=0), exiting\n");
        sc1(SYS_exit_group, 0);
        __builtin_unreachable();
    }

    /* Find PT_DYNAMIC in the program's program headers.
     * at_phdr points to the mapped phdrs of the main program. */
    Elf64_Phdr *phdr = (Elf64_Phdr *)at_phdr;
    Elf64_Dyn *dynamic = NULL;
    uint64_t prog_base = 0; /* for ET_DYN programs */

    /* Determine the program's load base. For ET_DYN, the first PT_LOAD
     * vaddr is usually 0, so at_phdr minus the phdr file offset gives us
     * the base. We can also compute it from the first PT_PHDR. */
    for (uint64_t i = 0; i < at_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)phdr + i * at_phent);
        if (ph->p_type == PT_PHDR) {
            prog_base = at_phdr - ph->p_vaddr;
            break;
        }
        if (ph->p_type == PT_LOAD && ph->p_offset == 0) {
            /* Fallback: first LOAD covering offset 0 */
            prog_base = at_phdr - ph->p_vaddr - /* assuming phdrs at small offset */ 0;
            /* More precisely: prog_base = at_phdr's page - ph->p_vaddr's page,
             * but for ELF files the phdrs are within the first LOAD, so:
             * base = mapping_start = at_phdr - (phdr_file_offset) where
             * phdr_file_offset = e_phoff. We don't have e_phoff directly,
             * but we know the ELF header is at prog_base + 0. */
        }
    }

    /* If no PT_PHDR found, compute from AT_PHDR assuming standard layout.
     * For a typical PIE, e_phoff = 0x40, and AT_PHDR = base + 0x40. */
    if (prog_base == 0 && at_phdr > 0x40) {
        /* Peek at the ELF header: the phdrs are at e_phoff from the file start.
         * Try reading the ELF header at at_phdr - some offset. The standard
         * e_phoff for 64-bit ELF is 0x40 (right after the header). */
        Elf64_Ehdr *maybe_eh = (Elf64_Ehdr *)(at_phdr - 0x40);
        if (maybe_eh->e_ident[0] == 0x7f && maybe_eh->e_ident[1] == 'E' &&
            maybe_eh->e_ident[2] == 'L'  && maybe_eh->e_ident[3] == 'F') {
            prog_base = at_phdr - maybe_eh->e_phoff;
        }
    }

    puts("  prog_base="); put_hex(prog_base); puts("\n");

    /* Find PT_DYNAMIC */
    for (uint64_t i = 0; i < at_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)phdr + i * at_phent);
        if (ph->p_type == PT_DYNAMIC) {
            dynamic = (Elf64_Dyn *)(prog_base + ph->p_vaddr);
            break;
        }
    }

    if (!dynamic) {
        /* Static binary that has PT_INTERP but no PT_DYNAMIC.
         * Nothing to link — just jump to entry. */
        puts("ld-cosmo: no PT_DYNAMIC, jumping to entry\n");
        goto jump;
    }

    /* Parse the program's .dynamic section */
    lib_t prog_lib;
    const char *needed[8];
    int needed_count = 0;
    parse_dynamic(dynamic, prog_base, &prog_lib, needed, &needed_count, 8);

    /* Extract relocation tables */
    Elf64_Rela *rela = NULL, *jmprel = NULL;
    uint64_t relasz = 0, pltrelsz = 0;
    for (Elf64_Dyn *d = dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:     rela = (Elf64_Rela *)(prog_base + d->d_val); break;
        case DT_RELASZ:   relasz = d->d_val; break;
        case DT_JMPREL:   jmprel = (Elf64_Rela *)(prog_base + d->d_val); break;
        case DT_PLTRELSZ: pltrelsz = d->d_val; break;
        default: break;
        }
    }

    /* Load DT_NEEDED libraries */
    lib_t libs[8];
    int lib_count = 0;

    for (int i = 0; i < needed_count; i++) {
        puts("ld-cosmo: DT_NEEDED: ");
        puts(needed[i]);
        puts("\n");
        if (load_library(needed[i], &libs[lib_count]) == 0) {
            lib_count++;
        } else {
            puts("ld-cosmo: STUB — library not loaded, continuing\n");
        }
    }

    /* Apply relocations to the main program */
    if (rela && relasz)
        apply_rela(rela, relasz / sizeof(Elf64_Rela), prog_base,
                   prog_lib.symtab, prog_lib.strtab, libs, lib_count);
    if (jmprel && pltrelsz)
        apply_rela(jmprel, pltrelsz / sizeof(Elf64_Rela), prog_base,
                   prog_lib.symtab, prog_lib.strtab, libs, lib_count);

    /* Expose main program and DT_NEEDED libs to dlsym(RTLD_DEFAULT) */
    dl_main_lib = prog_lib;
    dl_main_lib_valid = 1;
    for (int i = 0; i < lib_count && i < 8; i++)
        dl_needed_libs[i] = libs[i];
    dl_needed_libs_count = lib_count < 8 ? lib_count : 8;

    puts("ld-cosmo: relocations applied, jumping to entry\n");

jump:
    /* Jump to the program's _start.
     * We must NOT use a regular call — _start expects RSP to point
     * to the original argc/argv/envp/auxv layout. We saved sp at
     * function entry; restore it and jump. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "xor %%rdx, %%rdx\n\t"     /* zero rtld_fini (ABI convention) */
        "jmp *%1\n\t"
        :: "r"(sp), "r"(at_entry)
        : "memory"
    );
    __builtin_unreachable();
}
