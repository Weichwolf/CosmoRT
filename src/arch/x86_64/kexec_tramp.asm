; CosmoRT kexec trampoline — copied to physical 0x8000, runs from identity map
;
; After all processes are dead and APs halted, BSP copies this to 0x8000
; and jumps here. We switch to identity-mapped CR3, set RDI = boot_info,
; and jump to the new kernel entry point.
;
; Input (passed via data area at 0x8F00):
;   +0x00: new kernel entry (physical address)
;   +0x08: boot_info pointer (physical address)
;   +0x10: PML4 physical address (identity-mapped page tables)

bits 64
org 0x8000

kexec_tramp:
    cli

    ; Load data area at 0x8F00
    mov rax, [0x8F00]       ; entry point
    mov rdi, [0x8F08]       ; boot_info (physical)
    mov rcx, [0x8F10]       ; PML4 (physical)

    ; Switch to identity-mapped page tables
    mov cr3, rcx

    ; Temporary stack in trampoline page
    mov rsp, 0x8FF0

    ; Clear segment registers
    xor edx, edx
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    ; Mask LAPIC timer to prevent stale interrupts
    mov edx, 0xFEE00320     ; LAPIC_TIMER register (physical, identity-mapped)
    mov dword [rdx], 0x10000 ; masked

    ; Jump to new kernel entry
    jmp rax
