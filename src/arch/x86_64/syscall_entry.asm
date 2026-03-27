; CosmoRT SYSCALL entry — saves ALL user registers, frame pointer in percpu
;
; Saved frame layout (from RSP after all pushes):
;   [RSP+0x00]: r15     [RSP+0x08]: r14     [RSP+0x10]: r13
;   [RSP+0x18]: r12     [RSP+0x20]: rbp     [RSP+0x28]: rbx
;   [RSP+0x30]: r9      [RSP+0x38]: r8      [RSP+0x40]: r10
;   [RSP+0x48]: rdx     [RSP+0x50]: rsi     [RSP+0x58]: rdi
;   [RSP+0x60]: rax     [RSP+0x68]: r11(RFLAGS)  [RSP+0x70]: rcx(RIP)
;
; percpu offsets: gs:0=kernel_rsp, gs:8=user_rsp, gs:32=syscall_frame

bits 64
default rel
section .text

global syscall_entry_asm
extern sys_handler

syscall_entry_asm:
    swapgs
    mov [gs:8], rsp             ; save user RSP
    mov rsp, [gs:0]             ; load kernel RSP

    ; Save ALL user registers BEFORE enabling interrupts
    push rcx                    ; +0x70: user RIP
    push r11                    ; +0x68: user RFLAGS
    push rax                    ; +0x60: syscall number
    push rdi                    ; +0x58: arg1
    push rsi                    ; +0x50: arg2
    push rdx                    ; +0x48: arg3
    push r10                    ; +0x40: arg4
    push r8                     ; +0x38: arg5
    push r9                     ; +0x30: arg6
    push rbx                    ; +0x28
    push rbp                    ; +0x20
    push r12                    ; +0x18
    push r13                    ; +0x10
    push r14                    ; +0x08
    push r15                    ; +0x00

    ; Store frame pointer THEN enable interrupts (P0 fix: sti race)
    mov [gs:32], rsp
    stac                            ; SMAP: allow user memory access during syscall
    sti

    ; Call sys_handler(num, a1, a2, a3, a4, a5, a6)
    ; Read args from saved frame on stack
    mov rdi, [rsp + 0x60]       ; num = saved rax
    mov rsi, [rsp + 0x58]       ; a1 = saved rdi
    mov rdx, [rsp + 0x50]       ; a2 = saved rsi
    mov rcx, [rsp + 0x48]       ; a3 = saved rdx
    mov r8,  [rsp + 0x40]       ; a4 = saved r10
    mov r9,  [rsp + 0x38]       ; a5 = saved r8
    ; a6 = saved r9 → push on stack as 7th C arg
    push qword [rsp + 0x30]     ; r9 saved at RSP+0x30
    call sys_handler
    add rsp, 8                  ; pop a6

    ; Restore all registers (RAX = return value, skip saved rax)
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
    add rsp, 8                  ; skip saved rax
    cli
    clac                            ; SMAP: revoke user memory access
    pop r11                     ; user RFLAGS
    pop rcx                     ; user RIP

    mov rsp, [gs:8]             ; restore user RSP
    swapgs
    o64 sysret
