/* context.S — context_switch + setjmp/longjmp + Ring 3 entry for CosmoRT
 *
 * GDT selectors: user CS = 0x2B (0x28|3), user SS = 0x23 (0x20|3)
 */

.intel_syntax noprefix
.text

/* thread_t.kstack_rsp offset (verified by _Static_assert in sched.c) */
#define THREAD_KSTACK_RSP 232

/* ── context_switch(prev, next) ───────────────────────
 * Saves prev's callee-saved regs + RFLAGS + RSP, restores next's.
 * THE one context-switch mechanism. Symmetric. */
.globl context_switch
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

/* ── kernel_setjmp(buf) ───────────────────────────── */
.globl kernel_setjmp
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

/* ── kernel_longjmp(buf, val) ─────────────────────── */
.globl kernel_longjmp
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
    jnz 1f
    inc eax
1:  jmp [rdi+56]

/* ── ret_from_fork ────────────────────────────────────
 * context_switch returns here for new threads.
 * RSP → syscall_entry-compatible frame (15 regs).
 * Sets percpu state, loads fork return value, falls into syscall_return.
 *
 * percpu: gs:0=kernel_rsp, gs:8=user_rsp, gs:16=current_thread, gs:32=syscall_frame
 * thread_t: offset 8 = user rsp
 * Frame: [r15..rdi,rax,r11,rcx] — rax at offset 96 */
.globl ret_from_fork
ret_from_fork:
    mov gs:[32], rsp              /* percpu->syscall_frame */
    mov rax, gs:[16]              /* percpu->current_thread */
    mov rax, [rax + 8]            /* thread->rsp (user RSP) */
    mov gs:[8], rax               /* percpu->user_rsp */
    mov rax, [rsp + 96]           /* saved rax (fork return value) */
    jmp syscall_return

/* ── proc_enter_ring3(thread_t *t) ────────────────────
 * IRET to Ring 3. Reads register state from thread_t offsets.
 * Used only by execve (self-transformation, not context-switch). */
.globl proc_enter_ring3
proc_enter_ring3:
    push 0x23                     /* SS (user data, RPL=3) */
    push QWORD PTR [rdi + 8]     /* RSP */
    push QWORD PTR [rdi + 24]    /* RFLAGS */
    push 0x2B                     /* CS (user code 64, RPL=3) */
    push QWORD PTR [rdi + 16]    /* RIP */

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
