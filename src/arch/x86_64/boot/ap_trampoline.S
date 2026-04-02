/* ap_trampoline.S — AP 16→32→64 mode transition
 * Copied to physical 0x8000, started via SIPI vector 0x08.
 * NOTE: assembled as flat binary, must be position-aware at 0x8000.
 *
 * Data at 0x8F00 (filled by BSP):
 *   +0x00: PML4 physical address (uint64_t)
 *   +0x08: Stack base, direct-map (uint64_t)
 *   +0x10: Entry point, direct-map (uint64_t)
 *   +0x18: GDT: limit(u16) + base(u64)
 *   +0x28: Stack size per core (uint64_t)
 *   +0x30: IDT: limit(u16) + base(u64)
 */

.intel_syntax noprefix

/* ── 16-bit real mode ────────────────────────────── */
.code16
.globl ap_trampoline_start
ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [tramp_gdt_ptr]

    /* Protected mode */
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ljmp 0x08, pm32

/* ── 32-bit protected mode ───────────────────────── */
.code32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* PAE + PML4 + Long Mode + Paging */
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    mov eax, [0x8F00]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ljmp 0x18, lm64

/* ── 64-bit long mode ────────────────────────────── */
.code64
lm64:
    mov rsp, 0x8EF0               /* temporary stack */

    /* Load kernel GDT + IDT */
    lgdt [0x8F18]
    lidt [0x8F30]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* Reload CS */
    push 0x08
    lea rax, [rip + .Lcs_ok]
    push rax
    lretq

.Lcs_ok:
    /* APIC ID */
    mov rax, 0xFEE00020
    mov eax, [rax]
    shr eax, 24
    and eax, 0xFF

    /* Stack = stack_base + (apic_id + 1) * stack_size */
    mov ecx, eax
    inc rcx
    imul rcx, [0x8F28]
    add rcx, [0x8F08]
    mov rsp, rcx

    /* Jump to AP entry */
    mov rax, [0x8F10]
    jmp rax

    cli
.Lhang:
    hlt
    jmp .Lhang

/* ── Transitional GDT ────────────────────────────── */
.balign 8
tramp_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF      /* 0x08: 32-bit code */
    .quad 0x00CF92000000FFFF      /* 0x10: 32-bit data */
    .quad 0x00AF9A000000FFFF      /* 0x18: 64-bit code (L=1) */
tramp_gdt_end:

tramp_gdt_ptr:
    .word tramp_gdt_end - tramp_gdt - 1
    .long tramp_gdt

.globl ap_trampoline_end
ap_trampoline_end:
