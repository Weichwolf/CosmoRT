/* CosmoRT Memory Operations */

#include "memops.h"
#include "arch.h"

int memops_has_erms   = 0;
int memops_has_avx2   = 0;
int memops_has_rdrand = 0;

void memops_init(void) {
    /* CPUID feature detection for random.c (RDRAND) */
    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 7: ERMS (bit 9), AVX2 (bit 5) */
    arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1u << 9)) memops_has_erms = 1;
    /* AVX2 detection skipped — not used in C path */

    /* CPUID leaf 1: RDRAND (bit 30 of ECX) */
    arch_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (ecx & (1u << 30)) memops_has_rdrand = 1;
}

void page_zero(void *page) {
    unsigned long *p = (unsigned long *)page;
    for (int i = 0; i < 4096 / (int)sizeof(unsigned long); i++)
        p[i] = 0;
}

void pages_zero(void *base, int n) {
    unsigned char *p = (unsigned char *)base;
    for (int i = 0; i < n; i++) {
        page_zero(p);
        p += 4096;
    }
}

void kmemcpy(void *dst, const void *src, size_t len) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* Word-at-a-time for aligned, large copies */
    if (len >= 8 && ((unsigned long)d & 7) == ((unsigned long)s & 7)) {
        /* Align to 8-byte boundary */
        while ((unsigned long)d & 7) {
            *d++ = *s++;
            len--;
        }
        unsigned long *dw = (unsigned long *)d;
        const unsigned long *sw = (const unsigned long *)s;
        while (len >= 8) {
            *dw++ = *sw++;
            len -= 8;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }

    while (len--)
        *d++ = *s++;
}

void kmemset(void *dst, int val, size_t len) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)val;

    /* Word-at-a-time for large fills */
    if (len >= 8) {
        unsigned long vw = v;
        vw |= vw << 8;
        vw |= vw << 16;
        vw |= vw << 32;

        while ((unsigned long)d & 7) {
            *d++ = v;
            len--;
        }
        unsigned long *dw = (unsigned long *)d;
        while (len >= 8) {
            *dw++ = vw;
            len -= 8;
        }
        d = (unsigned char *)dw;
    }

    while (len--)
        *d++ = v;
}
