/* CosmoRT Memory Operations — SIMD-accelerated where available
 *
 * Feature detection at boot via CPUID:
 *   Baseline: SSE2 (always on x86_64)
 *   Optional: AVX2 (Haswell+), ERMS (Ivy Bridge+)
 *
 * No AVX-512: frequency throttling penalty > bandwidth gain for kernel use.
 */
#ifndef MEMOPS_H
#define MEMOPS_H

#include <stdint.h>
#include <stddef.h>

/* Detect CPU features. Call once at boot. */
void memops_init(void);

/* Zero one 4KB page. Optimal: REP STOSQ (ERMS) or MOVNTDQ (SSE2). */
void page_zero(void *page);

/* Zero N contiguous 4KB pages. Uses AVX2 VMOVNTDQ if available for large runs. */
void pages_zero(void *base, int n);

/* Copy memory. Uses REP MOVSB (ERMS) for large, unrolled MOV for small. */
void kmemcpy(void *dst, const void *src, size_t len);

/* Set memory to byte value. */
void kmemset(void *dst, int val, size_t len);

/* Feature flags (set by memops_init) */
extern int memops_has_erms;   /* Enhanced REP MOVSB/STOSB */
extern int memops_has_avx2;   /* AVX2 256-bit SIMD */
extern int memops_has_rdrand; /* RDRAND instruction */

#endif
