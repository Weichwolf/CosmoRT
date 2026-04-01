; context.asm — context_switch + setjmp/longjmp + Ring 3 entry for CosmoRT
;
; GDT selectors: user CS = 0x2B (0x28|3), user SS = 0x23 (0x20|3)

bits 64
section .text

global context_switch
global kernel_setjmp
global kernel_longjmp
global proc_enter_ring3

; thread_t.kstack_rsp offset (verified by _Static_assert in sched.c)
THREAD_KSTACK_RSP equ 232

; void context_switch(thread_t *prev, thread_t *next)
; Saves prev's callee-saved regs + RFLAGS + RSP, restores next's.
; This is THE one context-switch mechanism. Symmetric: every thread
; leaves and resumes through this exact point.
context_switch:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi + THREAD_KSTACK_RSP], rsp
    mov rsp, [rsi + THREAD_KSTACK_RSP]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
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

; ret_from_fork: context_switch returns here for new threads.
; RSP points to a syscall_entry_asm-compatible frame (15 regs).
; Sets percpu state, loads fork return value into RAX,
; and falls into syscall_return (sysret to userspace).
;
; percpu layout: gs:0=kernel_rsp, gs:8=user_rsp, gs:16=current_thread, gs:32=syscall_frame
; thread_t: offset 8 = user rsp
; Syscall frame: [r15,r14,r13,r12,rbp,rbx,r9,r8,r10,rdx,rsi,rdi,rax,r11,rcx]
;                offset 0                                       96  104  112
extern syscall_return
global ret_from_fork
ret_from_fork:
    ; Set percpu->syscall_frame = RSP (pointing to our fake frame)
    mov [gs:32], rsp

    ; Set percpu->user_rsp from current_thread->rsp (offset 8)
    mov rax, [gs:16]          ; rax = percpu->current_thread
    mov rax, [rax + 8]        ; rax = thread->rsp (user RSP)
    mov [gs:8], rax           ; percpu->user_rsp

    ; Load fork return value from saved rax slot in the frame.
    ; syscall_return skips saved rax (add rsp,8), so we must put it in RAX.
    mov rax, [rsp + 96]       ; rax = saved rax (0 for child, pid for parent)

    ; Fall through to syscall return path:
    ; pops r15..rdi, skips saved rax, cli, clac, pops r11+rcx, sysret
    jmp syscall_return

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
