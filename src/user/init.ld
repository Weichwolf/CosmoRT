/* Linker script for CosmoRT init — static ELF at 0x400000 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss  : { *(.bss*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
