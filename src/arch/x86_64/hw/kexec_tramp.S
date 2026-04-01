/* kexec_tramp.S — copied to physical 0x8000, runs from identity map
 *
 * Input (data area at 0x8F00):
 *   +0x00: new kernel entry (physical address)
 *   +0x08: boot_info pointer (physical address)
 *   +0x10: PML4 physical address (identity-mapped page tables)
 */

.intel_syntax noprefix
.text

.globl kexec_tramp
kexec_tramp:
    cli

    mov rax, [0x8F00]             /* entry point */
    mov rdi, [0x8F08]             /* boot_info (physical) */
    mov rcx, [0x8F10]             /* PML4 (physical) */

    mov cr3, rcx

    mov rsp, 0x8FF0               /* temporary stack */

    xor edx, edx
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    /* Mask LAPIC timer to prevent stale interrupts */
    mov edx, 0xFEE00320
    mov DWORD PTR [rdx], 0x10000

    jmp rax
