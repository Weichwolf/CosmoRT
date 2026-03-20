; CosmoRT Memory Operations — SIMD-accelerated hot paths
;
; page_zero:  1 page (4KB)  → REP STOSQ (ERMS) or SSE2 MOVNTDQ
; pages_zero: N pages        → AVX2 VMOVNTDQ for >=8 pages, else SSE2
; kmemcpy:    any size        → REP MOVSB (ERMS-optimized)
; kmemset:    any size        → REP STOSB
;
; Feature detection via CPUID, stored in globals.

bits 64
default rel
section .data

global memops_has_erms
global memops_has_avx2

memops_has_erms: dd 0
memops_has_avx2: dd 0

section .text

; ── memops_init: CPUID feature detection ─────────────
global memops_init
memops_init:
    push rbx

    ; Check ERMS (CPUID.07H:EBX bit 9)
    mov eax, 7
    xor ecx, ecx
    cpuid
    test ebx, (1 << 9)
    jz .no_erms
    mov dword [rel memops_has_erms], 1
.no_erms:

    ; Check AVX2 (CPUID.07H:EBX bit 5)
    ; ebx still has the value from above
    test ebx, (1 << 5)
    jz .no_avx2
    ; Also need OS XSAVE support (CR4.OSXSAVE + XCR0 bits 1,2)
    mov eax, 1
    cpuid
    test ecx, (1 << 27)    ; OSXSAVE
    jz .no_avx2
    xor ecx, ecx
    xgetbv                  ; XCR0 → EDX:EAX
    and eax, 0x06           ; SSE state (bit 1) + AVX state (bit 2)
    cmp eax, 0x06
    jne .no_avx2
    mov dword [rel memops_has_avx2], 1
.no_avx2:

    pop rbx
    ret

; ── page_zero(void *page) ───────────────────────────
; RDI = page address (4096 bytes, 4K-aligned)
; Uses SSE2 non-temporal stores (avoids cache pollution)
global page_zero
page_zero:
    pxor    xmm0, xmm0
    mov     ecx, 256            ; 256 × 16 = 4096
.pz_loop:
    movntdq [rdi],      xmm0
    movntdq [rdi + 16], xmm0   ; 2 stores per iter = 32B
    add     rdi, 32
    sub     ecx, 2
    jnz     .pz_loop
    sfence                      ; flush write-combine buffers
    ret

; ── pages_zero(void *base, int n) ───────────────────
; RDI = base address, ESI = number of 4KB pages
; For n >= 8 and AVX2 available: use VMOVNTDQ (32-byte stores)
; Otherwise: call page_zero per page
global pages_zero
pages_zero:
    push    rbx
    push    r12
    mov     r12d, esi           ; save page count
    mov     rbx, rdi            ; save base

    ; Check if AVX2 available and enough pages to amortize
    cmp     r12d, 8
    jl      .pz_pages_sse
    cmp     dword [rel memops_has_avx2], 0
    je      .pz_pages_sse

    ; ── AVX2 path: 32-byte non-temporal stores ──
    vpxor   ymm0, ymm0, ymm0
.pz_avx_page:
    mov     rdi, rbx
    mov     ecx, 64             ; 64 × 64 = 4096 bytes per page
.pz_avx_inner:
    vmovntdq [rdi],      ymm0
    vmovntdq [rdi + 32], ymm0  ; 64 bytes per iteration
    add     rdi, 64
    dec     ecx
    jnz     .pz_avx_inner
    add     rbx, 4096
    dec     r12d
    jnz     .pz_avx_page
    sfence
    vzeroupper                  ; avoid SSE/AVX transition penalty
    jmp     .pz_pages_done

    ; ── SSE2 fallback: page_zero per page ──
.pz_pages_sse:
    test    r12d, r12d
    jz      .pz_pages_done
    mov     rdi, rbx
    call    page_zero
    add     rbx, 4096
    dec     r12d
    jmp     .pz_pages_sse

.pz_pages_done:
    pop     r12
    pop     rbx
    ret

; ── kmemcpy(void *dst, const void *src, size_t len) ──
; RDI = dst, RSI = src, RDX = len
; REP MOVSB is ERMS-optimized on Ivy Bridge+. Even without ERMS,
; REP MOVSB is decent on modern CPUs. For < 64 bytes: unrolled MOV.
global kmemcpy
kmemcpy:
    mov     rcx, rdx
    cmp     rcx, 64
    jb      .kmc_small

    ; Large copy: REP MOVSB
    rep     movsb
    ret

.kmc_small:
    ; Unrolled copy for small sizes
    test    rcx, rcx
    jz      .kmc_done
.kmc_byte:
    mov     al, [rsi]
    mov     [rdi], al
    inc     rsi
    inc     rdi
    dec     rcx
    jnz     .kmc_byte
.kmc_done:
    ret

; ── kmemset(void *dst, int val, size_t len) ──────────
; RDI = dst, ESI = byte value, RDX = len
global kmemset
kmemset:
    mov     al, sil             ; byte value
    mov     rcx, rdx            ; length
    rep     stosb
    ret
