/* CRT0 for freestanding userspace binaries (ktest, e1000d, svcmgr).
 * SysV ABI: RSP is 16-byte aligned at _start entry, argc at (%rsp).
 * GCC-compiled C _start expects RSP = 16n+8 (as if entered via 'call').
 * This stub bridges the gap: align RSP then 'call' the C entry point. */
.text
.global _start
_start:
    xor  %rbp, %rbp       /* Clear frame pointer (ABI: first frame) */
    and  $-16, %rsp        /* Ensure 16-byte alignment (should be already) */
    call _start_c          /* call pushes 8 bytes → RSP = 16n+8 at callee */
    ud2                    /* unreachable */
