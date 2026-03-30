/* CosmoRT Memory Operations — SIMD-accelerated where available */
#ifndef MEMOPS_H
#define MEMOPS_H

#include <stdint.h>
#include <stddef.h>

void memops_init(void);

void page_zero(void *page);

void pages_zero(void *base, int n);

void kmemcpy(void *dst, const void *src, size_t len);

void kmemset(void *dst, int val, size_t len);

extern int memops_has_erms;
extern int memops_has_avx2;
extern int memops_has_rdrand;

#endif
