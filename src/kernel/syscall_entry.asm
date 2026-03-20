; CosmoRT SYSCALL entry — swapgs-based, saves ALL user registers
;
; On SYSCALL: RCX=user RIP, R11=user RFLAGS, RAX=syscall number
; Args: RDI, RSI, RDX, R10, R8, R9
;
; All user registers except RAX (return value) are preserved across syscall.
; This matches Linux behavior: user code only expects RCX, R11, RAX to change.

bits 64
default rel
section .text

global syscall_entry_asm
extern sys_handler

syscall_entry_asm:
    swapgs
    mov [gs:8], rsp             ; percpu.user_rsp = user RSP
    mov rsp, [gs:0]             ; RSP = percpu.kernel_rsp

    sti

    ; Save ALL user registers (including caller-saved)
    push rcx                    ; user RIP
    push r11                    ; user RFLAGS
    push rax                    ; syscall number (orig_rax)
    push rdi                    ; arg1
    push rsi                    ; arg2
    push rdx                    ; arg3
    push r10                    ; arg4
    push r8                     ; arg5
    push r9                     ; arg6
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Set up C call: sys_handler(num, a1, a2, a3, a4, a5, a6)
    ; Stack: [r15 r14 r13 r12 rbp rbx r9 r8 r10 rdx rsi rdi rax r11 rcx]
    ; We need: RDI=num(rax), RSI=a1(rdi), RDX=a2(rsi), RCX=a3(rdx), R8=a4(r10), R9=a5(r8)
    ;          [stack]=a6(r9)

    ; 7th arg (a6 = original r9) on stack for C
    push qword [rsp + 6*8]     ; r9 is at offset 6*8 from current RSP (after 15 pushes + this push)
    ; Actually let me recalculate. After 15 pushes, RSP points to r15.
    ; r9 is at RSP + 6*8 (r15=0, r14=8, r13=16, r12=24, rbp=32, rbx=40, r9=48)
    ; But we just did another push, so it's at RSP + 7*8 = RSP + 56

    ; Ugh, this is getting messy. Let me use a cleaner approach:
    ; Save args to local regs first, then shuffle.

    ; Pop the incorrect push
    add rsp, 8

    ; Args are still in their original positions on stack.
    ; Read them from stack instead of shuffling in registers.
    ; After 15 pushes: RSP points to r15 (top of stack)
    ; Layout from RSP: r15(0) r14(8) r13(16) r12(24) rbp(32) rbx(40)
    ;                  r9(48) r8(56) r10(64) rdx(72) rsi(80) rdi(88)
    ;                  rax(96) r11(104) rcx(112)

    ; Push a6 (r9) for 7th C argument
    push qword [rsp + 48]      ; r9 at offset 48

    ; Now set up C registers from stack values
    mov rdi, [rsp + 96 + 8]    ; rax (syscall num) — +8 for the push we just did
    mov rsi, [rsp + 88 + 8]    ; rdi (arg1)
    mov rdx, [rsp + 80 + 8]    ; rsi (arg2)
    mov rcx, [rsp + 72 + 8]    ; rdx (arg3)
    mov r8,  [rsp + 64 + 8]    ; r10 (arg4)
    mov r9,  [rsp + 56 + 8]    ; r8 (arg5)

    call sys_handler
    ; RAX = return value

    add rsp, 8                  ; pop a6

    ; Restore ALL user registers (except RAX which has return value)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    add rsp, 8                  ; skip saved rax (replaced by return value in rax)

    cli

    pop r11                     ; user RFLAGS
    pop rcx                     ; user RIP

    mov rsp, [gs:8]             ; restore user RSP
    swapgs
    o64 sysret
