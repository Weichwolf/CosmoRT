; context.asm — setjmp/longjmp + context_switch + Ring 3 entry for CosmoRT
;
; GDT selectors: user CS = 0x2B (0x28|3), user SS = 0x23 (0x20|3)

%define THREAD_KSTACK_RSP_OFF 168

bits 64
section .text

global kernel_setjmp
global kernel_longjmp
global context_switch
global context_save
global context_resume
global proc_enter_ring3

; void context_switch(thread_t *prev, thread_t *next)
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi + THREAD_KSTACK_RSP_OFF], rsp
    mov rsp, [rsi + THREAD_KSTACK_RSP_OFF]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; int context_save(thread_t *t)
context_save:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi + THREAD_KSTACK_RSP_OFF], rsp
    add rsp, 48
    xor eax, eax
    ret

; noreturn context_resume(thread_t *t)
context_resume:
    mov rsp, [rdi + THREAD_KSTACK_RSP_OFF]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    mov eax, 1
    ret

; int kernel_setjmp(uint64_t buf[8])
kernel_setjmp:
    mov [rdi],    rbx
    mov [rdi+8],  rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    mov [rdi+48], rsp
    mov rax, [rsp]
    mov [rdi+56], rax
    xor eax, eax
    ret

; void kernel_longjmp(uint64_t buf[8], int val)
kernel_longjmp:
    mov rbx, [rdi]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    mov rax, rsi
    test eax, eax
    jnz .nonzero
    inc eax
.nonzero:
    jmp [rdi+56]

; void proc_enter_ring3(process_t *p)
; process_t offsets: 8:rsp, 16:rip, 24:rflags,
;   32:rax, 40:rbx, 48:rcx, 56:rdx, 64:rsi, 72:rdi, 80:rbp,
;   88:r8, 96:r9, 104:r10, 112:r11, 120:r12, 128:r13, 136:r14, 144:r15
proc_enter_ring3:
    push qword 0x23          ; SS (user data, RPL=3)
    push qword [rdi + 8]     ; RSP
    push qword [rdi + 24]    ; RFLAGS
    push qword 0x2B          ; CS (user code 64, RPL=3)
    push qword [rdi + 16]    ; RIP

    mov r15, [rdi + 144]
    mov r14, [rdi + 136]
    mov r13, [rdi + 128]
    mov r12, [rdi + 120]
    mov r11, [rdi + 112]
    mov r10, [rdi + 104]
    mov r9,  [rdi + 96]
    mov r8,  [rdi + 88]
    mov rbp, [rdi + 80]
    mov rsi, [rdi + 64]
    mov rdx, [rdi + 56]
    mov rcx, [rdi + 48]
    mov rbx, [rdi + 40]
    mov rax, [rdi + 32]
    mov rdi, [rdi + 72]

    iretq
