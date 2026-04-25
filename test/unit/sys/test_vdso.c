/* vDSO ktest — verify __vdso_clock_gettime is reachable, returns sane
 * values, and matches the syscall path within tight tolerances. */

#include "ktest.h"

#define AT_NULL          0
#define AT_SYSINFO_EHDR 33

extern unsigned long  __crt0_argc;
extern unsigned long *__crt0_auxv;

/* Minimal ELF64 reader — find the first symbol whose name starts with
 * the given prefix in the AT_SYSINFO_EHDR DSO. Returns its absolute
 * runtime address or 0 on miss. The vDSO is mapped at a fixed VA so the
 * symbol's st_value (linked at the same VA) is already absolute. */
static unsigned long vdso_find_sym(const char *target) {
    unsigned long *auxv = __crt0_auxv;
    if (!auxv) return 0;
    unsigned long ehdr_va = 0;
    for (unsigned long *p = auxv; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_SYSINFO_EHDR) { ehdr_va = p[1]; break; }
    }
    if (!ehdr_va) return 0;

    const unsigned char *base = (const unsigned char *)ehdr_va;
    if (base[0] != 0x7F || base[1] != 'E' || base[2] != 'L' || base[3] != 'F')
        return 0;

    unsigned long e_phoff     = *(const unsigned long *)(base + 32);
    unsigned short e_phentsize = *(const unsigned short *)(base + 54);
    unsigned short e_phnum    = *(const unsigned short *)(base + 56);

    /* Find PT_DYNAMIC (type=2) to locate dynsym + dynstr */
    unsigned long dyn_off = 0, dyn_size = 0;
    for (int i = 0; i < e_phnum; i++) {
        const unsigned char *ph = base + e_phoff + (unsigned long)i * e_phentsize;
        unsigned int p_type = *(const unsigned int *)ph;
        if (p_type == 2) {
            dyn_off  = *(const unsigned long *)(ph + 16);   /* p_vaddr */
            dyn_size = *(const unsigned long *)(ph + 40);   /* p_memsz */
            break;
        }
    }
    if (!dyn_off) return 0;

    /* Walk dynamic table: tag/value pairs, end at DT_NULL=0. */
    const unsigned long *dyn = (const unsigned long *)dyn_off;
    unsigned long symtab = 0, strtab = 0;
    for (unsigned i = 0; i < dyn_size / 16; i++) {
        unsigned long tag = dyn[i * 2];
        unsigned long val = dyn[i * 2 + 1];
        if (tag == 0) break;             /* DT_NULL */
        if (tag == 5) strtab = val;      /* DT_STRTAB */
        if (tag == 6) symtab = val;      /* DT_SYMTAB */
    }
    if (!symtab || !strtab) return 0;

    /* Walk dynsym: 24-byte entries. st_name (4), st_info (1), st_other (1),
     * st_shndx (2), st_value (8), st_size (8). Stop at the first symbol
     * whose name starts with `target`. We don't have a defined upper bound
     * on the dynsym count from PT_DYNAMIC alone, so cap at 64 — way more
     * than the vDSO ever exports. */
    const char *strs = (const char *)strtab;
    for (int i = 0; i < 64; i++) {
        const unsigned char *sym = (const unsigned char *)(symtab + (unsigned long)i * 24);
        unsigned int st_name = *(const unsigned int *)sym;
        unsigned long st_value = *(const unsigned long *)(sym + 8);
        if (st_name == 0 && st_value == 0 && i > 0) break;     /* end of table */
        const char *name = strs + st_name;
        const char *t = target;
        while (*t && *name && *name == *t) { name++; t++; }
        if (*t == 0) return st_value;
    }
    return 0;
}

typedef int (*vdso_clock_gettime_fn)(int clk_id, void *ts);

static long ts_diff_ns(long sa, long na, long sb, long nb) {
    long d = (sb - sa) * 1000000000L + (nb - na);
    if (d < 0) d = -d;
    return d;
}

static void test_vdso(void) {
    puts("\n[vDSO]\n");

    /* 1. AT_SYSINFO_EHDR must be present in auxv. */
    unsigned long ehdr = 0;
    for (unsigned long *p = __crt0_auxv; p && p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_SYSINFO_EHDR) { ehdr = p[1]; break; }
    }
    check("AT_SYSINFO_EHDR present in auxv", ehdr != 0);
    if (!ehdr) return;

    /* 2. ELF magic at the auxv pointer. */
    const unsigned char *base = (const unsigned char *)ehdr;
    check("vDSO base has ELF magic",
          base[0] == 0x7F && base[1] == 'E' && base[2] == 'L' && base[3] == 'F');

    /* 3. __vdso_clock_gettime symbol is found by name. */
    unsigned long fn_addr = vdso_find_sym("__vdso_clock_gettime");
    check("__vdso_clock_gettime symbol resolvable", fn_addr != 0);
    if (!fn_addr) return;
    vdso_clock_gettime_fn fn = (vdso_clock_gettime_fn)fn_addr;

    /* 4. vDSO call returns 0 + sane CLOCK_MONOTONIC value. */
    struct { long sec; long nsec; } vts = { 0, 0 };
    int rc = fn(CLOCK_MONOTONIC, &vts);
    check_val("vdso CLOCK_MONOTONIC returns 0", rc, 0);
    check("vdso monotonic.sec >= 0 && < 3600", vts.sec >= 0 && vts.sec < 3600);
    check("vdso monotonic.nsec in range", vts.nsec >= 0 && vts.nsec < 1000000000);

    /* 5. vDSO ≈ syscall path (delta < 5ms). Read both back-to-back. */
    struct { long sec; long nsec; } sts;
    fn(CLOCK_MONOTONIC, &vts);
    long sysrc = sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&sts);
    check_val("syscall CLOCK_MONOTONIC returns 0", sysrc, 0);
    long delta_ns = ts_diff_ns(vts.sec, vts.nsec, sts.sec, sts.nsec);
    check("vdso vs syscall delta < 5ms", delta_ns < 5000000L);

    /* 6. CLOCK_REALTIME also works. */
    rc = fn(CLOCK_REALTIME, &vts);
    check_val("vdso CLOCK_REALTIME returns 0", rc, 0);
    check("vdso realtime.sec > 1700000000", vts.sec > 1700000000L);

    /* 7. Monotonicity over 1000 calls. */
    long prev_s = 0, prev_n = 0;
    int monotonic = 1;
    for (int i = 0; i < 1000; i++) {
        struct { long sec; long nsec; } t;
        fn(CLOCK_MONOTONIC, &t);
        if (i > 0) {
            long now = t.sec * 1000000000L + t.nsec;
            long old = prev_s * 1000000000L + prev_n;
            if (now < old) { monotonic = 0; break; }
        }
        prev_s = t.sec; prev_n = t.nsec;
    }
    check("vdso CLOCK_MONOTONIC is monotonic over 1000 calls", monotonic);

    /* 8. Performance: 1000 vDSO calls should complete in < 1ms (rdtsc only). */
    struct { long sec; long nsec; } t0, t1;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t0);
    for (int i = 0; i < 1000; i++) fn(CLOCK_MONOTONIC, &vts);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    long elapsed_ns = ts_diff_ns(t0.sec, t0.nsec, t1.sec, t1.nsec);
    puts("  1000 vdso calls: "); put_int(elapsed_ns / 1000); puts(" us\n");
    /* Generous bound (10ms) — passes anywhere except heavily-instrumented QEMU. */
    check("1000 vdso calls < 10ms", elapsed_ns < 10000000L);
}

TEST("vdso", test_vdso);
