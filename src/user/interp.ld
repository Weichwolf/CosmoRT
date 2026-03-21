/* Linker script for CosmoRT dynamic linker — static ELF at high address */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)
SECTIONS {
    . = 0x7F0000100000;
    .text : { *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss  : { *(.bss*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
