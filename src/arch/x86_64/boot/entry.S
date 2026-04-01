/* entry.S — CosmoRT Kernel Entry (called after ExitBootServices)
 * Sets up GDT, IDT, Page Tables, then jumps to C kernel_main
 *
 * GDT layout for SYSRET compatibility:
 *   0x00: null
 *   0x08: kernel code 64 (DPL=0)
 *   0x10: kernel data    (DPL=0)
 *   0x18: user code 32   (DPL=3, placeholder for SYSRET)
 *   0x20: user data      (DPL=3)
 *   0x28: user code 64   (DPL=3)
 *   0x30: TSS low
 *   0x38: TSS high
 *
 * SYSRET with STAR[63:48]=0x18:
 *   CS = (0x18+16)|3 = 0x2B  (user code 64)
 *   SS = (0x18+8)|3  = 0x23  (user data)
 */

.intel_syntax noprefix
.text

.globl kernel_entry

kernel_entry:
    /* Check if BSP or AP via LAPIC APIC ID */
    mov rax, 0xFEE00020
    mov eax, [rax]
    shr eax, 24
    test eax, eax
    jnz ap_halt

    /* Save boot_info pointer */
    mov rbx, rdi

    cli

    /* Load our GDT */
    lea rax, [rip + gdt_ptr]
    lgdt [rax]

    /* Reload segment registers */
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* Far jump to reload CS */
    push 0x08
    lea rax, [rip + .Lreload_cs]
    push rax
    .byte 0x48, 0xCB              /* retfq (REX.W far return) */

.Lreload_cs:
    lea rsp, [rip + stack_top]

    lea rax, [rip + idt_ptr]
    lidt [rax]

    /* Set up identity-mapped page tables (initial 8GB using 2MB pages) */
    lea rdi, [rip + pml4]
    xor eax, eax
    mov ecx, (512 + 512 + 4096) * 2
    rep stosd

    /* PML4[0] → PDPT */
    lea rdi, [rip + pml4]
    lea rax, [rip + pdpt]
    or rax, 0x03
    mov [rdi], rax

    /* PDPT[0..7] → 8 PD pages (8GB) */
    lea rdi, [rip + pdpt]
    lea rax, [rip + pd]
    or rax, 0x03
    mov ecx, 8
.Lfill_pdpt:
    mov [rdi], rax
    add rdi, 8
    add rax, 4096
    dec ecx
    jnz .Lfill_pdpt

    /* Fill PD: 4096 entries × 2MB = 8GB */
    lea rdi, [rip + pd]
    xor eax, eax
    mov ecx, 4096
.Lfill_pd:
    mov rdx, rax
    or rdx, 0x83                  /* present + writable + PS (2MB) */
    mov [rdi], rdx
    add rdi, 8
    add rax, 0x200000
    dec ecx
    jnz .Lfill_pd

    /* Mirror identity map to higher half: PML4[256] = PML4[0] */
    lea rdi, [rip + pml4]
    mov rax, [rdi]
    mov [rdi + 256*8], rax

    lea rax, [rip + pml4]
    mov cr3, rax

    /* Jump to higher-half direct map */
    lea rax, [rip + .Lin_high_half]
    mov rcx, 0xFFFF800000000000
    add rax, rcx
    jmp rax

.Lin_high_half:
    mov rcx, 0xFFFF800000000000
    add rsp, rcx
    add rbx, rcx

    /* Reload GDT with direct-map address */
    lea rax, [rip + gdt]
    lea rdi, [rip + gdt_ptr_high]
    mov [rdi + 2], rax
    lgdt [rdi]

    /* Reload segments */
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push 0x08
    lea rax, [rip + .Lreload_cs_high]
    push rax
    .byte 0x48, 0xCB              /* retfq (REX.W far return) */
.Lreload_cs_high:

    /* Reload IDT */
    lea rax, [rip + idt]
    lea rdi, [rip + idt_ptr_high]
    mov [rdi + 2], rax
    lidt [rdi]

    /* Initialize COM1 serial (115200 baud, 8N1) */
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x01
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al

    /* Print "K" on serial */
    mov dx, 0x3F8
.Lwait_tx:
    push dx
    mov dx, 0x3FD
    in al, dx
    pop dx
    test al, 0x20
    jz .Lwait_tx
    mov al, 'K'
    out dx, al

    /* Call kernel_main */
    mov rdi, rbx
    call kernel_main

.Lhalt:
    hlt
    jmp .Lhalt

/* ── GDT ──────────────────────────────────────────── */
.balign 16
gdt:
    .quad 0x0000000000000000      /* 0x00: null */
    .quad 0x00AF9A000000FFFF      /* 0x08: kernel code 64 */
    .quad 0x00CF92000000FFFF      /* 0x10: kernel data */
    .quad 0x00AFFA000000FFFF      /* 0x18: user code 32 (placeholder) */
    .quad 0x00CFF2000000FFFF      /* 0x20: user data */
    .quad 0x00AFFA000000FFFF      /* 0x28: user code 64 */
/* Per-core TSS descriptors (16 bytes each, filled at runtime) */
.globl tss_desc
tss_desc:
.rept 8
    .quad 0                       /* TSS low */
    .quad 0                       /* TSS high */
.endr
gdt_end:

gdt_ptr:
    .word gdt_end - gdt - 1
    .quad gdt

gdt_ptr_high:
    .word gdt_end - gdt - 1
    .quad 0                       /* patched in .Lin_high_half */

/* ── IDT ──────────────────────────────────────────── */
.balign 16
idt:
    .fill 256*2, 8, 0
idt_end:

idt_ptr:
    .word idt_end - idt - 1
    .quad idt

idt_ptr_high:
    .word idt_end - idt - 1
    .quad 0                       /* patched in .Lin_high_half */

/* ── Page Tables ──────────────────────────────────── */
.balign 4096
.globl pml4, pdpt, pd
pml4:   .fill 512, 8, 0
pdpt:   .fill 512, 8, 0
pd:     .fill 4096, 8, 0         /* 8 PD pages for initial 8GB */

/* ── AP support ───────────────────────────────────── */
.globl ap_go, ap_entry_addr, ap_stack_ptr, ap_cr3
ap_go:         .quad 0
ap_entry_addr: .quad 0
ap_stack_ptr:  .quad 0
ap_cr3:        .quad 0

ap_halt:
    pause
    lea rdi, [rip + ap_go]
    mov rax, [rdi]
    test rax, rax
    jz ap_halt
    lea rdi, [rip + ap_cr3]
    mov rax, [rdi]
    test rax, rax
    jz .Lskip_cr3
    mov cr3, rax
.Lskip_cr3:
    lea rdi, [rip + ap_stack_ptr]
    mov rsp, [rdi]
    lea rdi, [rip + ap_entry_addr]
    mov rax, [rdi]
    jmp rax

/* ── Stack ────────────────────────────────────────── */
.balign 16
stack_bottom:
    .space 262144                 /* 256KB stack */
stack_top:
