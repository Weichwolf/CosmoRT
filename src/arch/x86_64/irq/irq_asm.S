/* irq_asm.S — ISR stubs + common handler
 * Each stub: push dummy error code (if needed), push vector, jump to common
 */

.intel_syntax noprefix
.altmacro

.text

.globl isr_saved_rax

.bss
isr_saved_rax: .space 8

.text

/* ── Common ISR handler ─────────────────────────────
 * Stack on entry: [SS, RSP, RFLAGS, CS, RIP, error_code, vector_num]
 * We save ALL general-purpose registers for context switching. */
isr_common:
    /* SWAPGS if coming from Ring 3 */
    test QWORD PTR [rsp + 24], 3
    jz 1f
    swapgs
1:
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

    lea rax, [rip + isr_saved_rax]
    mov rdx, [rsp + 112]
    mov [rax], rdx

    mov rdi, [rsp + 120]          /* vector number */
    mov rsi, rsp                  /* frame pointer */
    call irq_dispatch

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

    /* SWAPGS if returning to Ring 3 */
    test QWORD PTR [rsp + 24], 3
    jz 1f
    swapgs
1:
    add rsp, 16
    iretq

/* ── Stub generation ──────────────────────────────── */

.macro ISR_NOERR num
isr_stub_\num:
    push 0
    push \num
    jmp isr_common
.endm

.macro ISR_ERR num
isr_stub_\num:
    push \num
    jmp isr_common
.endm

/* Exceptions 0-31 */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* IRQs 32-255 */
.set i, 32
.rept 224
ISR_NOERR %i
.set i, i+1
.endr

/* ── Stub address table ───────────────────────────── */
.macro STUB_ENTRY num
    .quad isr_stub_\num
.endm

.data
.globl isr_stub_table
isr_stub_table:
.set i, 0
.rept 256
STUB_ENTRY %i
.set i, i+1
.endr
