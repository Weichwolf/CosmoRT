/* CRT0 for freestanding userspace binaries (ktest, e1000d, svcmgr).
 * SysV ABI: RSP is 16-byte aligned at _start entry, argc at (%rsp),
 * then argv[], NULL, envp[], NULL, auxv[].
 *
 * Capture pointers into globals (__crt0_argc/argv/envp/auxv) so test code
 * can access AT_SYSINFO_EHDR & friends. _start_c remains a void(void) entry
 * for compatibility with existing test/main.c and src/user/init.c. */
.text
.global _start
.global __crt0_argc
.global __crt0_argv
.global __crt0_envp
.global __crt0_auxv

_start:
    xor  %rbp, %rbp                 /* Clear frame pointer (ABI: first frame) */

    /* RSP layout on entry: [argc][argv0][argv1]...[NULL][envp0]...[NULL][auxv...][AT_NULL] */
    movq (%rsp), %rax               /* argc */
    movq %rax, __crt0_argc(%rip)
    leaq 8(%rsp), %rdi              /* argv = &stack[1] */
    movq %rdi, __crt0_argv(%rip)

    /* envp = argv + argc + 1 (skip NULL terminator) */
    leaq 8(%rdi, %rax, 8), %rsi     /* envp = argv + (argc+1)*8 */
    movq %rsi, __crt0_envp(%rip)

    /* auxv starts after envp[]'s NULL terminator. Scan envp for NULL. */
    movq %rsi, %rcx
1:  cmpq $0, (%rcx)
    leaq 8(%rcx), %rcx              /* advance past current entry */
    jne 1b                          /* keep going while not NULL */
    movq %rcx, __crt0_auxv(%rip)    /* points just past envp NULL */

    and  $-16, %rsp                 /* Ensure 16-byte alignment */
    call _start_c                   /* call pushes 8 bytes -> RSP = 16n+8 at callee */
    ud2                             /* unreachable */

.bss
.align 8
__crt0_argc: .quad 0
__crt0_argv: .quad 0
__crt0_envp: .quad 0
__crt0_auxv: .quad 0
