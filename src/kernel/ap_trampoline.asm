; CosmoRT AP Trampoline — 16-bit real → 32-bit → 64-bit long mode
;
; Copied to physical 0x8000. APs start here after SIPI (vector 0x08).
; Data at 0x8F00 filled by BSP.

bits 16
org 0x8000

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Signal arrival: write marker to 0x8FF0
    mov byte [0x8FF0], 0xAA

    ; Load transitional GDT
    lgdt [tramp_gdt_ptr]

    ; Enable protected mode
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp dword 0x08:pm32

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Enable PAE
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    ; Load PML4
    mov eax, [0x8F00]
    mov cr3, eax

    ; Enable long mode (IA32_EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; Far jump to 64-bit
    jmp dword 0x18:lm64

bits 64
lm64:
    ; Load kernel GDT
    mov rax, 0x8F18
    lgdt [rax]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS
    push qword 0x08
    mov rax, 0x8000 + (.cs_ok - ap_trampoline_start)
    push rax
    retfq

.cs_ok:
    ; Get APIC ID
    mov rax, 0xFEE00020
    mov eax, [rax]
    shr eax, 24
    and eax, 0xFF

    ; Compute stack: stack_base + (apic_id + 1) * stack_size
    mov ecx, eax
    inc rcx
    mov rax, 0x8F28
    imul rcx, [rax]
    mov rax, 0x8F08
    add rcx, [rax]
    mov rsp, rcx

    ; Jump to AP entry
    mov rax, 0x8F10
    jmp [rax]

; ── Transitional GDT ──────────────────────────────────
align 8
tramp_gdt:
    dq 0x0000000000000000       ; 0x00: null
    dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF       ; 0x10: 32-bit data
    dq 0x00AF9A000000FFFF       ; 0x18: 64-bit code (L=1)
tramp_gdt_end:

tramp_gdt_ptr:
    dw tramp_gdt_end - tramp_gdt - 1
    dd tramp_gdt

ap_trampoline_end:
