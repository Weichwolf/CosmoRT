; ISR stubs + common handler
; Each stub: push dummy error code (if needed), push vector, jump to common

bits 64
default rel
section .text

extern irq_dispatch
global isr_saved_rax
section .bss
isr_saved_rax: resq 1
section .text

; ── Common ISR handler ─────────────────────────────
; Stack on entry: [SS, RSP, RFLAGS, CS, RIP, error_code, vector_num]
; We save ALL general-purpose registers for context switching.
isr_common:
    ; SWAPGS if coming from Ring 3 (user mode)
    ; CS is at [RSP+24] (vector_num + error_code + RIP, then CS)
    test qword [rsp + 24], 3
    jz .no_swapgs_entry
    swapgs
.no_swapgs_entry:
    ; Save all GPRs (15 registers)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save RAX for syscall number extraction
    mov [rel isr_saved_rax], rax

    ; arg1 (rdi) = vector number
    ; 15 pushes * 8 = 120 bytes, then vector_num at +120
    mov rdi, [rsp + 120]

    ; arg2 (rsi) = pointer to saved register frame on stack
    mov rsi, rsp

    call irq_dispatch

    ; Restore all GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; SWAPGS if returning to Ring 3
    test qword [rsp + 24], 3
    jz .no_swapgs_exit
    swapgs
.no_swapgs_exit:
    add rsp, 16           ; pop vector_num + error_code
    iretq

; ── Stub macros ────────────────────────────────────
; No error code: push 0 as dummy, then vector
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0          ; dummy error code
    push qword %1         ; vector number
    jmp isr_common
%endmacro

; With error code (CPU already pushed it): just push vector
%macro ISR_ERR 1
isr_stub_%1:
    push qword %1         ; vector number (error code already on stack)
    jmp isr_common
%endmacro

; ── Exception stubs (0-31) ─────────────────────────
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8   ; Double Fault
ISR_NOERR 9
ISR_ERR   10  ; Invalid TSS
ISR_ERR   11  ; Segment Not Present
ISR_ERR   12  ; Stack-Segment Fault
ISR_ERR   13  ; General Protection Fault
ISR_ERR   14  ; Page Fault
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17  ; Alignment Check
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21  ; Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29  ; VMM Communication
ISR_ERR   30  ; Security Exception
ISR_NOERR 31

; ── IRQ stubs (32-255) ─────────────────────────────
%assign i 32
%rep 224
ISR_NOERR i
%assign i i+1
%endrep

; ── Stub address table (for IDT setup in C) ────────
section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
