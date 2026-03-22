/* CosmoRT ELF Loader — static + dynamic ELF64 binaries */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

/* ELF64 types */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;
    Elf64_Off   e_phoff;
    Elf64_Off   e_shoff;
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;
    Elf64_Half  e_phentsize;
    Elf64_Half  e_phnum;
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

/* Segment types */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7

/* ELF magic */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/* e_type */
#define ET_EXEC 2
#define ET_DYN  3

/* e_machine */
#define EM_X86_64 62

/* Segment permission flags */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Auxiliary vector types */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_RANDOM  25

/* Extended ELF load result — carries info needed for PT_INTERP / auxv */
typedef struct {
    uint64_t entry;        /* entry point to jump to (interpreter's if PT_INTERP) */
    uint64_t prog_entry;   /* program's original e_entry (relocated for ET_DYN) */
    uint64_t prog_phdr;    /* program headers mapped address */
    uint16_t prog_phent;   /* sizeof(Elf64_Phdr) */
    uint16_t prog_phnum;   /* number of program headers */
    uint64_t interp_base;  /* interpreter load base (0 if no interpreter) */
    uint64_t brk;          /* page-aligned end of loaded segments */
    uint64_t stack_ptr;    /* initial RSP */
    uint64_t load_base;    /* base address used for ET_DYN (0 for ET_EXEC) */
    char     interp[256];  /* interpreter path from PT_INTERP ("" if none) */
} elf_info_t;

/* Load a static ELF64 binary into a user address space.
 * Sets up page tables, stack, argc/argv/envp/auxv.
 * Returns 0 on success, -1 on error.
 * On success: *entry = ELF entry point, *stack_ptr = initial RSP. */
int elf_load(const void *data, size_t len, uint64_t *pml4,
             uint64_t stack_top,
             uint64_t *entry, uint64_t *stack_ptr, uint64_t *brk_out);

/* Extended ELF load: maps segments + returns metadata for dynamic linking.
 * Does NOT set up the stack (caller does that with auxv info).
 * Supports ET_EXEC and ET_DYN (PIE, with ASLR base).
 * Returns 0 on success, -1 on error. */
int elf_load_ex(const void *data, size_t len, uint64_t *pml4,
                uint64_t base_hint, elf_info_t *info);

/* Load a static ELF64 from CosmoFS inode — no contiguous buffer needed.
 * Reads segments page-by-page from disk. */
int elf_load_cosmofs(uint64_t ino, uint64_t *user_pml4,
                     uint64_t stack_top,
                     uint64_t *entry, uint64_t *stack_ptr, uint64_t *brk_out);

/* Extended ELF load from CosmoFS inode — handles ET_EXEC + ET_DYN. */
int elf_load_ex_cosmofs(uint64_t ino, uint64_t *pml4,
                        uint64_t base_hint, elf_info_t *info);

#endif
