/* CosmoRT vDSO linker script — single LOAD segment from file offset 0.
 *
 * Linked at fixed VA VDSO_CODE_VA so musl-style readers can use the
 * AT_SYSINFO_EHDR pointer directly as the ELF base. ELF header lives at
 * VDSO_CODE_VA + 0; section content immediately after the program headers,
 * all in a single PT_LOAD segment with R+X permission. */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(__vdso_clock_gettime)

PHDRS {
    text    PT_LOAD    FILEHDR PHDRS FLAGS(5);   /* PF_R | PF_X, includes ELF header */
    dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS {
    . = 0x7FFFEFFFF000 + SIZEOF_HEADERS;
    .hash           : { *(.hash) }            :text
    .gnu.hash       : { *(.gnu.hash) }
    .dynsym         : { *(.dynsym) }
    .dynstr         : { *(.dynstr) }
    .gnu.version    : { *(.gnu.version) }
    .gnu.version_d  : { *(.gnu.version_d) }
    .gnu.version_r  : { *(.gnu.version_r) }
    .dynamic        : { *(.dynamic) }         :text :dynamic
    .rodata         : { *(.rodata*) }         :text
    .text           : { *(.text*) }           :text
    /DISCARD/       : { *(.note*) *(.comment) *(.eh_frame*) *(.got*) *(.plt*) }
}
