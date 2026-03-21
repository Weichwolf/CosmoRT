; CosmoRT Kernel Entry — called after ExitBootServices
; Sets up GDT, IDT, Page Tables, then jumps to C kernel_main
;
; GDT layout for SYSRET compatibility:
;   0x00: null
;   0x08: kernel code 64 (DPL=0)
;   0x10: kernel data    (DPL=0)
;   0x18: user code 32   (DPL=3, placeholder for SYSRET)
;   0x20: user data      (DPL=3)
;   0x28: user code 64   (DPL=3)
;   0x30: TSS low
;   0x38: TSS high
;
; SYSRET with STAR[63:48]=0x18:
;   CS = (0x18+16)|3 = 0x2B  (user code 64)
;   SS = (0x18+8)|3  = 0x23  (user data)

bits 64
default rel
section .text

global kernel_entry
extern kernel_main

kernel_entry:
    ; Check if BSP or AP via LAPIC APIC ID
    mov rax, 0xFEE00020
    mov eax, [rax]
    shr eax, 24
    test eax, eax
    jnz ap_halt

    ; Save boot_info pointer
    mov rbx, rdi

    cli

    ; Load our GDT
    lgdt [gdt_ptr]

    ; Reload segment registers
    mov ax, 0x10        ; kernel data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to reload CS
    push 0x08           ; kernel code
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    lea rsp, [rel stack_top]

    lidt [idt_ptr]

    ; Set up identity-mapped page tables (initial 8GB using 2MB pages)
    ; Enough to boot. paging_init extends to cover all physical RAM.
    lea rdi, [rel pml4]
    xor eax, eax
    mov ecx, (512 + 512 + 4096) * 2
    rep stosd

    ; PML4[0] → PDPT
    lea rdi, [rel pml4]
    lea rax, [rel pdpt]
    or rax, 0x03
    mov [rdi], rax

    ; PDPT[0..7] → 8 PD pages (8GB, enough for boot)
    lea rdi, [rel pdpt]
    lea rax, [rel pd]
    or rax, 0x03
    mov ecx, 8
.fill_pdpt:
    mov [rdi], rax
    add rdi, 8
    add rax, 4096
    dec ecx
    jnz .fill_pdpt

    ; Fill PD: 4096 entries × 2MB = 8GB
    lea rdi, [rel pd]
    xor eax, eax
    mov ecx, 4096
.fill_pd:
    mov rdx, rax
    or rdx, 0x83            ; present + writable + PS (2MB)
    mov [rdi], rdx
    add rdi, 8
    add rax, 0x200000
    dec ecx
    jnz .fill_pd

    ; Mirror identity map to higher half: PML4[256] = PML4[0]
    ; Both point to the same PDPT, so all 64 PDPT entries are shared
    lea rdi, [rel pml4]
    mov rax, [rdi]
    mov [rdi + 256*8], rax

    lea rax, [rel pml4]
    mov cr3, rax

    ; Jump to higher-half direct map (0xFFFF800000000000 + phys)
    lea rax, [rel .in_high_half]
    mov rcx, 0xFFFF800000000000
    add rax, rcx
    jmp rax

.in_high_half:
    ; Now running at direct-map address. Adjust RSP and boot_info pointer.
    mov rcx, 0xFFFF800000000000
    add rsp, rcx
    add rbx, rcx

    ; Reload GDT — lea is RIP-relative, already gives direct-map address
    lea rax, [rel gdt]
    lea rdi, [rel gdt_ptr_high]
    mov [rdi + 2], rax
    lgdt [rdi]

    ; Reload segments after new GDT
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push 0x08
    lea rax, [rel .reload_cs_high]
    push rax
    retfq
.reload_cs_high:

    ; Reload IDT — RIP-relative already in direct map
    lea rax, [rel idt]
    lea rdi, [rel idt_ptr_high]
    mov [rdi + 2], rax
    lidt [rdi]

    ; Initialize COM1 serial (115200 baud, 8N1)
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

    ; Print "K" on serial
    mov dx, 0x3F8
    mov al, 'K'
.wait_tx:
    push dx
    mov dx, 0x3FD
    in al, dx
    pop dx
    test al, 0x20
    jz .wait_tx
    mov al, 'K'
    out dx, al

    ; Call kernel_main
    mov rdi, rbx
    call kernel_main

.halt:
    hlt
    jmp .halt

; ── GDT ──────────────────────────────────────────────
align 16
gdt:
    dq 0x0000000000000000   ; 0x00: null
    dq 0x00AF9A000000FFFF   ; 0x08: kernel code 64 (DPL=0, L=1)
    dq 0x00CF92000000FFFF   ; 0x10: kernel data    (DPL=0)
    dq 0x00AFFA000000FFFF   ; 0x18: user code 32   (DPL=3, L=1, placeholder)
    dq 0x00CFF2000000FFFF   ; 0x20: user data      (DPL=3)
    dq 0x00AFFA000000FFFF   ; 0x28: user code 64   (DPL=3, L=1)
; TSS descriptor (16 bytes, filled at runtime by tss_init)
global tss_desc
tss_desc:
    dq 0                    ; 0x30: TSS low
    dq 0                    ; 0x38: TSS high
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dq gdt

; Higher-half GDT/IDT pointers (base patched at runtime)
gdt_ptr_high:
    dw gdt_end - gdt - 1
    dq 0                        ; patched in .in_high_half

; ── IDT ──────────────────────────────────────────────
align 16
idt:
    times 256 dq 0, 0
idt_end:

idt_ptr:
    dw idt_end - idt - 1
    dq idt

idt_ptr_high:
    dw idt_end - idt - 1
    dq 0                        ; patched in .in_high_half

; ── Page Tables ──────────────────────────────────────
align 4096
global pml4, pdpt, pd
pml4:   times 512 dq 0
pdpt:   times 512 dq 0
pd:     times 4096 dq 0    ; 8 PD pages for initial 8GB (extended by paging_init)

; ── AP support ───────────────────────────────────────
global ap_go, ap_entry_addr, ap_stack_ptr, ap_cr3
ap_go:         dq 0
ap_entry_addr: dq 0
ap_stack_ptr:  dq 0
ap_cr3:        dq 0

ap_halt:
    pause
    lea rdi, [rel ap_go]
    mov rax, [rdi]
    test rax, rax
    jz ap_halt
    ; Load kernel page tables (with higher-half mappings)
    lea rdi, [rel ap_cr3]
    mov rax, [rdi]
    test rax, rax
    jz .skip_cr3
    mov cr3, rax
.skip_cr3:
    lea rdi, [rel ap_stack_ptr]
    mov rsp, [rdi]
    lea rdi, [rel ap_entry_addr]
    mov rax, [rdi]
    jmp rax

; ── Stack ────────────────────────────────────────────
align 16
stack_bottom:
    times 262144 db 0       ; 256KB stack
stack_top:
