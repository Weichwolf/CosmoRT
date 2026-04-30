/* CosmoRT — Hardware primitive syscalls (SYS_COSMO_*) for userspace drivers.
 * Separate dispatch from Linux syscalls for clean jump-table generation. */

#include "internal.h"
#include "core/clocksource.h"
#include "core/timer.h"
#include "core/waitqueue.h"
#include "hw/acpi.h"
#include "hw/hyperv.h"
#include "hw/kvmclock.h"
#include "arch/hpet.h"
#include "arch/acpi_pm.h"
#include "config.h"
#include "memops.h"
#include "proc/thread.h"

#define HW_CAP_CHECK() do { \
    process_t *_p = proc_current(); \
    if (!_p || !_p->is_driver) return -EPERM; \
} while (0)

long do_cosmo_mmio_map(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    void *virt;
    int r = cosmo_mmio_map((uint64_t)a1, (size_t)a2, &virt);
    if (r == 0) { r = copy_to_user((void *)a3, &virt, sizeof(virt)); if (r) return r; }
    return r;
}

long do_cosmo_dma_alloc(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    void *virt; uint64_t phys;
    int r = cosmo_dma_alloc((size_t)a1, &virt, &phys);
    if (r == 0) {
        int r2 = copy_to_user((void *)a2, &virt, sizeof(virt)); if (r2) return r2;
        r2 = copy_to_user((void *)a3, &phys, sizeof(phys)); if (r2) return r2;
    }
    return r;
}

long do_cosmo_dma_free(long a1, long a2) {
    HW_CAP_CHECK();
    cosmo_dma_free((void *)a1, (size_t)a2);
    return 0;
}

long do_cosmo_irq_register(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    if ((uint64_t)a2 >= 0x800000000000ULL || a2 == 0) return -EFAULT;
    return cosmo_irq_register((int)a1, (void (*)(void *))a2, (void *)a3);
}

long do_cosmo_pci_read(long a1, long a2, long a3, long a4, long a5) {
    HW_CAP_CHECK();
    if (!user_ok(a5, 4)) return -EFAULT;
    return cosmo_pci_config_read((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t *)a5);
}

long do_cosmo_pci_write(long a1, long a2, long a3, long a4, long a5) {
    HW_CAP_CHECK();
    return cosmo_pci_config_write((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t)a5);
}

long do_cosmo_fw_load(long a1, long a2, long a3) {
    HW_CAP_CHECK();
    if (!user_ok(a1, 1) || !user_ok(a2, 8) || !user_ok(a3, 8)) return -EFAULT;
    return cosmo_fw_load((const char *)a1, (void **)a2, (size_t *)a3);
}

long do_cosmo_nic_attach(long a1) {
    HW_CAP_CHECK();
    struct { uint64_t shm_phys; uint64_t shm_size; uint8_t mac[6]; } kargs;
    { int r = copy_from_user(&kargs, (const void *)a1, sizeof(kargs)); if (r) return r; }
    return net_port_attach(kargs.shm_phys, (size_t)kargs.shm_size, kargs.mac);
}

long do_cosmo_kexec(long a1, long a2) {
    HW_CAP_CHECK();
    if (!user_ok(a1, (size_t)a2)) return -EFAULT;
    extern int do_kexec(const void *, size_t);
    return do_kexec((const void *)a1, (size_t)a2);
}

/* ── clocksource selftests (subcases 10-19) ─────────
 * In-kernel helpers exercising the clocksource/clock_event core. Each returns
 * 0 on success, negative on failure. Invoked from test/unit/sched/test_clocksource.c
 * via SYS_COSMO_RT_QUERY. Fake clocksources register with ratings above every
 * real clocksource (KVM_PVCLOCK/HYPERV_TSC = 400) and are always unregistered
 * before return so the live TSC stays current.
 *
 * Test ratings must be strictly above 400 to keep the rating-sort invariant
 * stable when kvmclock/hyperv_tsc are present (rating 400). They must also
 * stay <= CLOCKSOURCE_RATING_MAX = 499. */

#define CS_TEST_RATING_HIGH (CLOCKSOURCE_RATING_MAX)         /* 499 */
#define CS_TEST_RATING_MID  (CLOCKSOURCE_RATING_MAX - 25)    /* 474 */
#define CS_TEST_RATING_LOW  (CLOCKSOURCE_RATING_MAX - 50)    /* 449 */
_Static_assert(CS_TEST_RATING_LOW > CLOCKSOURCE_RATING_KVM_PVCLOCK,
               "test ratings must exceed every real clocksource rating");
_Static_assert(CS_TEST_RATING_HIGH <= CLOCKSOURCE_RATING_MAX,
               "test ratings must respect CLOCKSOURCE_RATING_MAX cap");
_Static_assert(CS_TEST_RATING_HIGH > CS_TEST_RATING_MID
               && CS_TEST_RATING_MID > CS_TEST_RATING_LOW,
               "test ratings must be strictly ordered for sort test");
#define CS_TEST_FREQ_PM     3579545ULL
#define CS_TEST_FREQ_TSC    2400000000ULL
#define CS_TEST_NS_PER_SEC  1000000000ULL
#define CS_TEST_TOLERANCE_PPM 100

static uint64_t cst_counter0, cst_counter1, cst_counter2;
static uint64_t cst_read0(void) { return cst_counter0; }
static uint64_t cst_read1(void) { return cst_counter1; }
static uint64_t cst_read2(void) { return cst_counter2; }

static int cst_within_ppm(uint64_t got, uint64_t expect, uint64_t ppm) {
    uint64_t diff = got > expect ? got - expect : expect - got;
    return (diff * 1000000ULL) <= (expect * ppm);
}

static long cst_rating_sort(void) {
    struct clocksource a = { .name = "cstA", .rating = CS_TEST_RATING_LOW,
                             .read = cst_read0, .mask = CLOCKSOURCE_MASK_64,
                             .mult = 1, .shift = 0 };
    struct clocksource b = { .name = "cstB", .rating = CS_TEST_RATING_HIGH,
                             .read = cst_read1, .mask = CLOCKSOURCE_MASK_64,
                             .mult = 1, .shift = 0 };
    struct clocksource c = { .name = "cstC", .rating = CS_TEST_RATING_MID,
                             .read = cst_read2, .mask = CLOCKSOURCE_MASK_64,
                             .mult = 1, .shift = 0 };
    clocksource_register(&a);
    clocksource_register(&b);
    clocksource_register(&c);
    struct clocksource *cur = clocksource_current();
    long rc = 0;
    if (!cur || cur != &b) rc = -1;
    else if (cur->next != &c) rc = -2;
    else if (cur->next->next != &a) rc = -3;
    clocksource_unregister(&a);
    clocksource_unregister(&b);
    clocksource_unregister(&c);
    return rc;
}

static long cst_register_higher(void) {
    struct clocksource *prev = clocksource_current();
    struct clocksource hi = { .name = "cstHi", .rating = CS_TEST_RATING_HIGH,
                              .read = cst_read0, .mask = CLOCKSOURCE_MASK_64,
                              .mult = 1, .shift = 0 };
    clocksource_register(&hi);
    long rc = (clocksource_current() == &hi) ? 0 : -1;
    clocksource_unregister(&hi);
    if (clocksource_current() != prev) rc = rc ? rc : -2;
    return rc;
}

static long cst_register_lower(void) {
    struct clocksource *prev = clocksource_current();
    if (!prev) return -1;
    struct clocksource lo = { .name = "cstLo", .rating = 2,
                              .read = cst_read0, .mask = CLOCKSOURCE_MASK_64,
                              .mult = 1, .shift = 0 };
    clocksource_register(&lo);
    long rc = (clocksource_current() == prev) ? 0 : -2;
    clocksource_unregister(&lo);
    return rc;
}

static long cst_unregister_current(void) {
    struct clocksource *prev = clocksource_current();
    struct clocksource hi = { .name = "cstUC", .rating = CS_TEST_RATING_HIGH,
                              .read = cst_read0, .mask = CLOCKSOURCE_MASK_64,
                              .mult = 1, .shift = 0 };
    clocksource_register(&hi);
    if (clocksource_current() != &hi) { clocksource_unregister(&hi); return -1; }
    clocksource_unregister(&hi);
    return (clocksource_current() == prev) ? 0 : -2;
}

static long cst_unregister_noncurrent(void) {
    struct clocksource *prev = clocksource_current();
    struct clocksource lo = { .name = "cstNC", .rating = 2,
                              .read = cst_read0, .mask = CLOCKSOURCE_MASK_64,
                              .mult = 1, .shift = 0 };
    clocksource_register(&lo);
    if (clocksource_current() != prev) { clocksource_unregister(&lo); return -1; }
    clocksource_unregister(&lo);
    return (clocksource_current() == prev) ? 0 : -2;
}

static long cst_mult_shift_pm(void) {
    uint32_t mult = 0, shift = 0;
    clocksource_calc_mult_shift(&mult, &shift, CS_TEST_FREQ_PM,
                                CS_TEST_NS_PER_SEC, 600);
    if (shift == 0 || mult == 0) return -1;
    uint64_t ns = ((uint64_t)CS_TEST_FREQ_PM * mult) >> shift;
    return cst_within_ppm(ns, CS_TEST_NS_PER_SEC, CS_TEST_TOLERANCE_PPM) ? 0 : -2;
}

static long cst_mult_shift_tsc(void) {
    uint32_t mult = 0, shift = 0;
    clocksource_calc_mult_shift(&mult, &shift, CS_TEST_FREQ_TSC,
                                CS_TEST_NS_PER_SEC, 600);
    if (shift == 0 || mult == 0) return -1;
    uint64_t ns = ((uint64_t)CS_TEST_FREQ_TSC * mult) >> shift;
    return cst_within_ppm(ns, CS_TEST_NS_PER_SEC, CS_TEST_TOLERANCE_PPM) ? 0 : -2;
}

static long cst_mask_wrap(void) {
    cst_counter0 = 0xfffffff0ULL;
    struct clocksource w = { .name = "cstW", .rating = CS_TEST_RATING_HIGH,
                             .read = cst_read0, .mask = CLOCKSOURCE_MASK_32,
                             .mult = 1, .shift = 0 };
    clocksource_register(&w);
    uint64_t ns_before = clocksource_read_ns();
    cst_counter0 = 0x00000010ULL;
    uint64_t ns_after = clocksource_read_ns();
    clocksource_unregister(&w);
    uint64_t delta = ns_after - ns_before;
    return (delta == 0x20ULL) ? 0 : -1;
}

static long cst_monotonic(void) {
    uint64_t t1 = clocksource_read_ns();
    uint64_t t2 = clocksource_read_ns();
    uint64_t t3 = clocksource_read_ns();
    if (t2 < t1) return -1;
    if (t3 < t2) return -2;
    return 0;
}

static volatile uint64_t cst_ce_last_delta;
static int cst_ce_set(uint64_t delta_ns) { cst_ce_last_delta = delta_ns; return 0; }

static long cst_clock_event(void) {
    struct clock_event_device ce = { .name = "cstCE", .rating = CS_TEST_RATING_HIGH,
                                     .features = CLOCK_EVT_FEAT_ONESHOT,
                                     .set_next_event = cst_ce_set };
    clock_event_register(&ce);
    if (clock_event_current() != &ce) { clock_event_unregister(&ce); return -1; }
    cst_ce_last_delta = 0;
    int r = clock_event_set_oneshot(123456ULL);
    clock_event_unregister(&ce);
    if (r != 0) return -2;
    if (cst_ce_last_delta != 123456ULL) return -3;
    return 0;
}

/* ── ACPI parser selftests (subcases 20-29) ─────────
 * Fake tables live in kernel BSS so virt_to_phys works; XSDT/RSDT entries
 * hold phys addrs, the parser maps them back via phys_to_virt. Every test
 * restores boot_info-derived state via acpi_restore_from_boot() before
 * return. */

#define AT_MAX_ENTRIES 4
#define AT_MAX_TABLES  3

struct at_root_xsdt {
    struct acpi_sdt_header hdr;
    uint64_t               entry[AT_MAX_ENTRIES];
} __attribute__((packed));

struct at_root_rsdt {
    struct acpi_sdt_header hdr;
    uint32_t               entry[AT_MAX_ENTRIES];
} __attribute__((packed));

struct at_madt_buf {
    struct acpi_madt       madt;
    uint8_t                body[128];
} __attribute__((packed));

static struct acpi_rsdp    at_rsdp;
static struct at_root_xsdt at_xsdt;
static struct at_root_rsdt at_rsdt;
static struct acpi_sdt_header at_tables[AT_MAX_TABLES];
static struct acpi_fadt    at_fadt;
static struct acpi_hpet    at_hpet;
static struct at_madt_buf  at_madt;

static void at_set_sig(char *dst, const char *sig, int len) {
    for (int i = 0; i < len; i++) dst[i] = sig[i];
}

static void at_fix_checksum(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    p[offsetof(struct acpi_sdt_header, checksum)] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    p[offsetof(struct acpi_sdt_header, checksum)] = (uint8_t)(-(int)sum);
}

static void at_build_sdt(struct acpi_sdt_header *h, const char *sig, uint32_t len) {
    kmemset(h, 0, len);
    at_set_sig(h->signature, sig, ACPI_SIG_LEN);
    h->length   = len;
    h->revision = 1;
    at_fix_checksum(h, len);
}

static void at_build_rsdp(struct acpi_rsdp *r, uint8_t revision,
                          uint32_t rsdt_phys, uint64_t xsdt_phys) {
    kmemset(r, 0, sizeof(*r));
    const char sig[ACPI_RSDP_SIG_LEN] = { 'R','S','D',' ','P','T','R',' ' };
    for (int i = 0; i < ACPI_RSDP_SIG_LEN; i++) r->signature[i] = sig[i];
    r->revision     = revision;
    r->rsdt_address = rsdt_phys;
    if (revision >= ACPI_REV_2) {
        r->length       = sizeof(*r);
        r->xsdt_address = xsdt_phys;
    }
    /* rev0 checksum over first 20 bytes */
    uint8_t *p = (uint8_t *)r;
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + p[i]);
    r->checksum = (uint8_t)(-(int)sum);
    if (revision >= ACPI_REV_2) {
        sum = 0;
        for (size_t i = 0; i < sizeof(*r); i++) sum = (uint8_t)(sum + p[i]);
        r->extended_checksum = (uint8_t)(-(int)sum);
    }
}

static long at_rsdp_validate(void) {
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, 0);
    if (!acpi_rsdp_validate(&at_rsdp)) return -1;
    at_rsdp.signature[0] = 'X';
    if (acpi_rsdp_validate(&at_rsdp)) return -2;
    return 0;
}

static long at_rsdp_checksum(void) {
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, 0);
    if (!acpi_rsdp_validate(&at_rsdp)) return -1;
    at_rsdp.checksum ^= 0xff;
    if (acpi_rsdp_validate(&at_rsdp)) return -2;
    /* rev2 but bad extended checksum */
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, 0);
    at_rsdp.extended_checksum ^= 0xff;
    if (acpi_rsdp_validate(&at_rsdp)) return -3;
    return 0;
}

static long at_revision_routing(void) {
    at_build_sdt(&at_tables[0], ACPI_SIG_FADT, sizeof(struct acpi_sdt_header));

    /* rev0 → RSDT */
    at_build_sdt(&at_rsdt.hdr, ACPI_SIG_RSDT,
                 sizeof(struct acpi_sdt_header) + 4);
    at_rsdt.entry[0] = (uint32_t)virt_to_phys(&at_tables[0]);
    at_rsdt.hdr.length = sizeof(struct acpi_sdt_header) + 4;
    at_fix_checksum(&at_rsdt, at_rsdt.hdr.length);

    at_build_rsdp(&at_rsdp, ACPI_REV_1, (uint32_t)virt_to_phys(&at_rsdt), 0);
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }
    const struct acpi_sdt_header *f = acpi_find_table(ACPI_SIG_FADT);
    long rc = (f == &at_tables[0]) ? 0 : -2;
    acpi_restore_from_boot();
    return rc;
}

static void at_build_xsdt_with(int n) {
    uint32_t len = sizeof(struct acpi_sdt_header) + (uint32_t)n * 8u;
    kmemset(&at_xsdt, 0, sizeof(at_xsdt));
    at_set_sig(at_xsdt.hdr.signature, ACPI_SIG_XSDT, ACPI_SIG_LEN);
    at_xsdt.hdr.length   = len;
    at_xsdt.hdr.revision = 1;
    for (int i = 0; i < n; i++)
        at_xsdt.entry[i] = virt_to_phys(&at_tables[i]);
    at_fix_checksum(&at_xsdt, len);
}

static long at_xsdt_walk_find(void) {
    at_build_sdt(&at_tables[0], ACPI_SIG_FADT, sizeof(struct acpi_sdt_header));
    at_build_sdt(&at_tables[1], ACPI_SIG_HPET, sizeof(struct acpi_sdt_header));
    at_build_sdt(&at_tables[2], ACPI_SIG_MCFG, sizeof(struct acpi_sdt_header));
    at_build_xsdt_with(3);
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }
    long rc = 0;
    if (acpi_find_table(ACPI_SIG_FADT) != &at_tables[0]) rc = -2;
    else if (acpi_find_table(ACPI_SIG_HPET) != &at_tables[1]) rc = -3;
    else if (acpi_find_table(ACPI_SIG_MCFG) != &at_tables[2]) rc = -4;
    acpi_restore_from_boot();
    return rc;
}

static long at_xsdt_walk_missing(void) {
    at_build_sdt(&at_tables[0], ACPI_SIG_FADT, sizeof(struct acpi_sdt_header));
    at_build_xsdt_with(1);
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }
    long rc = (acpi_find_table(ACPI_SIG_HPET) == 0) ? 0 : -2;
    acpi_restore_from_boot();
    return rc;
}

static long at_fadt_pm_tmr(void) {
    kmemset(&at_fadt, 0, sizeof(at_fadt));
    at_set_sig(at_fadt.header.signature, ACPI_SIG_FADT, ACPI_SIG_LEN);
    at_fadt.header.length   = sizeof(at_fadt);
    at_fadt.header.revision = 3;
    at_fadt.pm_timer_block  = 0xB008;
    at_fadt.pm_timer_length = 4;
    at_fadt.flags           = ACPI_FADT_FLAG_TMR_VAL_EXT;
    at_fix_checksum(&at_fadt, sizeof(at_fadt));

    /* install FADT as only XSDT entry */
    at_xsdt.entry[0] = virt_to_phys(&at_fadt);
    kmemset(&at_xsdt.hdr, 0, sizeof(at_xsdt.hdr));
    at_set_sig(at_xsdt.hdr.signature, ACPI_SIG_XSDT, ACPI_SIG_LEN);
    at_xsdt.hdr.length   = sizeof(struct acpi_sdt_header) + 8;
    at_xsdt.hdr.revision = 1;
    at_fix_checksum(&at_xsdt, at_xsdt.hdr.length);

    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }

    const struct acpi_sdt_header *h = acpi_find_table(ACPI_SIG_FADT);
    long rc = 0;
    if (!h) rc = -2;
    else {
        const struct acpi_fadt *f = (const struct acpi_fadt *)h;
        if (f->pm_timer_block != 0xB008) rc = -3;
        else if (f->pm_timer_length != 4) rc = -4;
        else if (!(f->flags & ACPI_FADT_FLAG_TMR_VAL_EXT)) rc = -5;
    }
    acpi_restore_from_boot();
    return rc;
}

static long at_hpet_fields(void) {
    kmemset(&at_hpet, 0, sizeof(at_hpet));
    at_set_sig(at_hpet.header.signature, ACPI_SIG_HPET, ACPI_SIG_LEN);
    at_hpet.header.length           = sizeof(at_hpet);
    at_hpet.header.revision         = 1;
    at_hpet.event_timer_block_id    = 0x8086A201;
    at_hpet.base_address.address    = 0xFED00000ULL;
    at_hpet.base_address.address_space_id = 0;
    at_hpet.hpet_number             = 0;
    at_hpet.min_clock_tick          = 14318;
    at_fix_checksum(&at_hpet, sizeof(at_hpet));

    at_xsdt.entry[0] = virt_to_phys(&at_hpet);
    kmemset(&at_xsdt.hdr, 0, sizeof(at_xsdt.hdr));
    at_set_sig(at_xsdt.hdr.signature, ACPI_SIG_XSDT, ACPI_SIG_LEN);
    at_xsdt.hdr.length   = sizeof(struct acpi_sdt_header) + 8;
    at_xsdt.hdr.revision = 1;
    at_fix_checksum(&at_xsdt, at_xsdt.hdr.length);

    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }

    const struct acpi_sdt_header *h = acpi_find_table(ACPI_SIG_HPET);
    long rc = 0;
    if (!h) rc = -2;
    else {
        const struct acpi_hpet *hp = (const struct acpi_hpet *)h;
        if (hp->base_address.address != 0xFED00000ULL) rc = -3;
        else if (hp->min_clock_tick != 14318) rc = -4;
    }
    acpi_restore_from_boot();
    return rc;
}

static long at_madt_counts(void) {
    kmemset(&at_madt, 0, sizeof(at_madt));
    at_set_sig(at_madt.madt.header.signature, ACPI_SIG_MADT, ACPI_SIG_LEN);
    at_madt.madt.header.revision    = 3;
    at_madt.madt.local_apic_address = 0xFEE00000;
    at_madt.madt.flags              = 1;

    uint8_t *p = at_madt.body;

    struct acpi_madt_lapic *la0 = (struct acpi_madt_lapic *)p;
    la0->entry.type = ACPI_MADT_TYPE_LAPIC;
    la0->entry.length = sizeof(*la0);
    la0->acpi_processor_id = 0;
    la0->apic_id = 0;
    la0->flags = 1;
    p += sizeof(*la0);

    struct acpi_madt_lapic *la1 = (struct acpi_madt_lapic *)p;
    la1->entry.type = ACPI_MADT_TYPE_LAPIC;
    la1->entry.length = sizeof(*la1);
    la1->acpi_processor_id = 1;
    la1->apic_id = 1;
    la1->flags = 1;
    p += sizeof(*la1);

    struct acpi_madt_lapic *la2 = (struct acpi_madt_lapic *)p;
    la2->entry.type = ACPI_MADT_TYPE_LAPIC;
    la2->entry.length = sizeof(*la2);
    la2->acpi_processor_id = 2;
    la2->apic_id = 2;
    la2->flags = 0;
    p += sizeof(*la2);

    struct acpi_madt_ioapic *io = (struct acpi_madt_ioapic *)p;
    io->entry.type = ACPI_MADT_TYPE_IOAPIC;
    io->entry.length = sizeof(*io);
    io->ioapic_id = 0;
    io->ioapic_address = 0xFEC00000;
    io->global_irq_base = 0;
    p += sizeof(*io);

    uint32_t madt_len = (uint32_t)(sizeof(struct acpi_madt) + (size_t)(p - at_madt.body));
    at_madt.madt.header.length = madt_len;
    at_fix_checksum(&at_madt, madt_len);

    at_xsdt.entry[0] = virt_to_phys(&at_madt);
    kmemset(&at_xsdt.hdr, 0, sizeof(at_xsdt.hdr));
    at_set_sig(at_xsdt.hdr.signature, ACPI_SIG_XSDT, ACPI_SIG_LEN);
    at_xsdt.hdr.length   = sizeof(struct acpi_sdt_header) + 8;
    at_xsdt.hdr.revision = 1;
    at_fix_checksum(&at_xsdt, at_xsdt.hdr.length);

    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }

    int lapic_cnt = acpi_madt_count(ACPI_MADT_TYPE_LAPIC);
    int ioapic_cnt = acpi_madt_count(ACPI_MADT_TYPE_IOAPIC);
    long rc = 0;
    if (lapic_cnt != 2) rc = -2;
    else if (ioapic_cnt != 1) rc = -3;
    acpi_restore_from_boot();
    return rc;
}

static long at_corrupt_sdt(void) {
    at_build_sdt(&at_tables[0], ACPI_SIG_FADT, sizeof(struct acpi_sdt_header));
    at_tables[0].checksum ^= 0xff;     /* break */
    at_build_xsdt_with(1);
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }
    long rc = (acpi_find_table(ACPI_SIG_FADT) == 0) ? 0 : -2;
    acpi_restore_from_boot();
    return rc;
}

static long at_duplicate_sig(void) {
    at_build_sdt(&at_tables[0], ACPI_SIG_FADT, sizeof(struct acpi_sdt_header));
    at_build_sdt(&at_tables[1], ACPI_SIG_FADT, sizeof(struct acpi_sdt_header));
    at_build_xsdt_with(2);
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) { acpi_restore_from_boot(); return -1; }
    long rc = (acpi_find_table(ACPI_SIG_FADT) == &at_tables[0]) ? 0 : -2;
    acpi_restore_from_boot();
    return rc;
}

static long at_checksum_helper(void) {
    uint8_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 0 };
    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) sum = (uint8_t)(sum + buf[i]);
    buf[7] = (uint8_t)(-(int)sum);
    if (!acpi_checksum_ok(buf, 8)) return -1;
    buf[0] ^= 1;
    if (acpi_checksum_ok(buf, 8)) return -2;
    return 0;
}

/* ── HPET selftests (subcases 40-44) ─────────────────
 * Uses hpet_override_base_for_test() to redirect MMIO access to a kernel
 * BSS buffer shaped as an HPET register file. No real HPET required. */

#define HPET_TEST_PERIOD_FS  69841279u           /* typical QEMU 14.318 MHz */
#define HPET_TEST_BUF_WORDS  64                  /* 512 bytes — covers up to timer 4 */

static uint64_t hpet_test_mmio[HPET_TEST_BUF_WORDS] __attribute__((aligned(16)));

static void hpet_test_reset_buf(uint32_t period_fs, int num_comparators,
                                int is_64bit) {
    for (int i = 0; i < HPET_TEST_BUF_WORDS; i++) hpet_test_mmio[i] = 0;
    uint64_t caps =
        ((uint64_t)period_fs << HPET_GCAP_PERIOD_SHIFT) |
        ((uint64_t)((num_comparators - 1) & HPET_GCAP_NUM_TIM_MASK)
             << HPET_GCAP_NUM_TIM_SHIFT) |
        (is_64bit ? HPET_GCAP_COUNT_SIZE : 0);
    hpet_test_mmio[HPET_REG_GCAP_ID / 8] = caps;
}

static inline uint64_t *hpet_test_reg(uint32_t off) {
    return &hpet_test_mmio[off / 8];
}

static long ht_period_to_freq(void) {
    hpet_test_reset_buf(HPET_TEST_PERIOD_FS, 3, 1);
    hpet_override_base_for_test((uint64_t)(uintptr_t)hpet_test_mmio,
                                HPET_TEST_PERIOD_FS, 1, 3);
    long rc = 0;
    uint64_t expected_hz = HPET_FEMTO_PER_SEC / (uint64_t)HPET_TEST_PERIOD_FS;
    if (hpet_frequency_hz() != expected_hz) rc = -1;
    else if (hpet_period_fs() != HPET_TEST_PERIOD_FS) rc = -2;
    else if (!hpet_counter_is_64bit()) rc = -3;
    else if (hpet_num_comparators() != 3) rc = -4;
    hpet_clear_override_for_test();
    return rc;
}

static long ht_read_counter(void) {
    hpet_test_reset_buf(HPET_TEST_PERIOD_FS, 1, 1);
    hpet_override_base_for_test((uint64_t)(uintptr_t)hpet_test_mmio,
                                HPET_TEST_PERIOD_FS, 1, 1);
    *hpet_test_reg(HPET_REG_MAIN_CNT) = 0x1122334455667788ULL;
    long rc = (hpet_read() == 0x1122334455667788ULL) ? 0 : -1;
    *hpet_test_reg(HPET_REG_MAIN_CNT) = 0x00000000aabbccddULL;
    if (!rc && hpet_read() != 0x00000000aabbccddULL) rc = -2;
    hpet_clear_override_for_test();
    return rc;
}

static long ht_clocksource_mask(void) {
    hpet_test_reset_buf(HPET_TEST_PERIOD_FS, 1, 1);
    hpet_override_base_for_test((uint64_t)(uintptr_t)hpet_test_mmio,
                                HPET_TEST_PERIOD_FS, 1, 1);
    long rc = 0;
    if (hpet_period_fs() != HPET_TEST_PERIOD_FS) rc = -1;
    else if (!hpet_counter_is_64bit()) rc = -2;
    hpet_clear_override_for_test();

    hpet_test_reset_buf(HPET_TEST_PERIOD_FS, 1, 0);
    hpet_override_base_for_test((uint64_t)(uintptr_t)hpet_test_mmio,
                                HPET_TEST_PERIOD_FS, 0, 1);
    if (!rc && hpet_counter_is_64bit()) rc = -3;
    hpet_clear_override_for_test();
    return rc;
}

static long ht_mult_shift_sanity(void) {
    hpet_test_reset_buf(HPET_TEST_PERIOD_FS, 1, 1);
    hpet_override_base_for_test((uint64_t)(uintptr_t)hpet_test_mmio,
                                HPET_TEST_PERIOD_FS, 1, 1);
    uint64_t hz = hpet_frequency_hz();
    uint32_t mult = 0, shift = 0;
    clocksource_calc_mult_shift(&mult, &shift, hz, CS_TEST_NS_PER_SEC, 600);
    long rc = 0;
    if (shift == 0 || mult == 0) rc = -1;
    else {
        uint64_t ns = (hz * mult) >> shift;
        if (!cst_within_ppm(ns, CS_TEST_NS_PER_SEC, CS_TEST_TOLERANCE_PPM))
            rc = -2;
    }
    hpet_clear_override_for_test();
    return rc;
}

static long ht_boot_probe(void) {
    /* On hosts exposing HPET via ACPI, hpet_init registered our clocksource
     * during boot. On hosts without HPET, the driver stays inert — in that
     * case the probe is a no-op success. */
    if (!hpet_available()) return 0;
    if (hpet_period_fs() < HPET_PERIOD_MIN_FS) return -1;
    if (hpet_period_fs() > HPET_PERIOD_MAX_FS) return -2;
    if (hpet_frequency_hz() == 0) return -3;
    if (hpet_num_comparators() < 1) return -4;

    struct clocksource *cs = clocksource_current();
    int found = 0;
    while (cs) {
        if (cs->name && cs->name[0] == 'h' && cs->name[1] == 'p'
            && cs->name[2] == 'e' && cs->name[3] == 't') { found = 1; break; }
        cs = cs->next;
    }
    if (!found) return -5;

    uint64_t a = hpet_read();
    for (volatile int i = 0; i < 1000; i++) { __asm__ volatile(""); }
    uint64_t b = hpet_read();
    if (hpet_counter_is_64bit() && b < a) return -6;
    return 0;
}

/* ── ACPI PM_TMR selftests (subcases 80-84) ──────────
 * Override-Hook leitet acpi_pm_read auf einen in-BSS uint32_t um. Reale
 * Port-IO wird in den Tests nie ausgeloest. Subcase 84 (boot probe) ist
 * bedingt: unter QEMU exponiert der PIIX4-PM/Q35 das Register; ohne
 * PM_TMR bleibt der Treiber inert und die Pruefung degradiert zu no-op. */

#define APM_TEST_PORT_FAKE    0xdead
#define APM_TEST_NAME_P0      'a'
#define APM_TEST_NAME_P1      'c'
#define APM_TEST_NAME_P2      'p'
#define APM_TEST_NAME_P3      'i'
#define APM_TEST_NAME_P4      '_'
#define APM_TEST_NAME_P5      'p'
#define APM_TEST_NAME_P6      'm'

static volatile uint32_t apm_test_src;

static int apm_name_equals_acpi_pm(const char *s) {
    return s && s[0] == APM_TEST_NAME_P0 && s[1] == APM_TEST_NAME_P1
            && s[2] == APM_TEST_NAME_P2 && s[3] == APM_TEST_NAME_P3
            && s[4] == APM_TEST_NAME_P4 && s[5] == APM_TEST_NAME_P5
            && s[6] == APM_TEST_NAME_P6 && s[7] == '\0';
}

static long apm_missing_fadt(void) {
    /* FADT entfernen: XSDT nur mit HPET → acpi_pm_init muss no-op bleiben.
     * apm_registered schuetzt die Boot-Registrierung vor Doppel-Einhaengung
     * beim abschliessenden acpi_pm_init(). */
    at_build_sdt(&at_tables[0], ACPI_SIG_HPET, sizeof(struct acpi_sdt_header));
    at_build_xsdt_with(1);
    at_build_rsdp(&at_rsdp, ACPI_REV_2, 0, virt_to_phys(&at_xsdt));
    if (acpi_override_rsdp_for_test(virt_to_phys(&at_rsdp)) != 0) {
        acpi_restore_from_boot();
        return -1;
    }

    acpi_pm_clear_override_for_test();
    acpi_pm_init();
    long rc = acpi_pm_available() ? -2 : 0;

    acpi_restore_from_boot();
    acpi_pm_init();
    return rc;
}

static long apm_mask_24_vs_32(void) {
    apm_test_src = 0x01ABCDEFu;
    acpi_pm_override_for_test(&apm_test_src, APM_TEST_PORT_FAKE, 0);
    long rc = 0;
    if (acpi_pm_is_32bit())               rc = -1;
    else if (acpi_pm_read() != 0x00ABCDEFu) rc = -2;
    acpi_pm_clear_override_for_test();

    acpi_pm_override_for_test(&apm_test_src, APM_TEST_PORT_FAKE, 1);
    if (!rc && !acpi_pm_is_32bit())       rc = -3;
    if (!rc && acpi_pm_read() != 0x01ABCDEFu) rc = -4;
    acpi_pm_clear_override_for_test();
    return rc;
}

static long apm_mult_shift_sanity(void) {
    uint32_t mult = 0, shift = 0;
    clocksource_calc_mult_shift(&mult, &shift, ACPI_PM_FREQ_HZ,
                                CS_TEST_NS_PER_SEC, ACPI_PM_MAX_SEC_WINDOW);
    if (shift == 0 || mult == 0) return -1;
    uint64_t ns = ((uint64_t)ACPI_PM_FREQ_HZ * mult) >> shift;
    return cst_within_ppm(ns, CS_TEST_NS_PER_SEC, CS_TEST_TOLERANCE_PPM) ? 0 : -2;
}

static long apm_monotonic(void) {
    apm_test_src = 0;
    acpi_pm_override_for_test(&apm_test_src, APM_TEST_PORT_FAKE, 1);
    long rc = 0;
    uint32_t prev = acpi_pm_read();
    for (uint32_t step = 1; step <= 8 && !rc; step++) {
        apm_test_src = prev + step;
        uint32_t cur = acpi_pm_read();
        if (cur < prev) rc = -1;
        prev = cur;
    }
    acpi_pm_clear_override_for_test();
    return rc;
}

static long apm_boot_probe(void) {
    if (!acpi_pm_available()) return 0;
    if (acpi_pm_port() == 0) return -1;

    struct clocksource *cs = clocksource_current();
    int found = 0;
    while (cs) {
        if (apm_name_equals_acpi_pm(cs->name)) {
            if (cs->rating != CLOCKSOURCE_RATING_ACPI_PM) return -2;
            if (cs->mask != (acpi_pm_is_32bit() ? ACPI_PM_MASK_32BIT
                                                : ACPI_PM_MASK_24BIT))
                return -3;
            if (cs->mult == 0 || cs->shift == 0) return -4;
            found = 1;
            break;
        }
        cs = cs->next;
    }
    if (!found) return -5;

    uint32_t a = acpi_pm_read();
    for (volatile int i = 0; i < 1000; i++) { __asm__ volatile(""); }
    uint32_t b = acpi_pm_read();
    uint64_t mask = acpi_pm_is_32bit() ? ACPI_PM_MASK_32BIT : ACPI_PM_MASK_24BIT;
    if ((uint64_t)a > mask || (uint64_t)b > mask) return -6;
    return 0;
}

/* ── Hyper-V TSC clocksource selftests (subcases 50-53) ─
 * Ohne Hyper-V-Enlightenment (QEMU default) liefert hyperv_detect()==0. In
 * dem Fall degradieren Cases 51/52 zu no-op success — das spiegelt die
 * Laufzeit-Semantik wider: nichts registriert, nichts lesbar, kein Bug. */

#define HVCS_NAME_FIRST_CHAR  'h'
#define HVCS_EXPECTED_RATING  CLOCKSOURCE_RATING_HYPERV_TSC

static int hvcs_name_equals_hyperv_tsc(const char *s) {
    const char *want = "hyperv_tsc";
    while (*want) { if (*s++ != *want++) return 0; }
    return *s == '\0';
}

static long hvcs_init_no_hyperv(void) {
    if (hyperv_detect()) return 0;
    hyperv_clocksource_init();
    struct clocksource *cs = clocksource_current();
    while (cs) {
        if (cs->name && cs->name[0] == HVCS_NAME_FIRST_CHAR
            && hvcs_name_equals_hyperv_tsc(cs->name))
            return -1;
        cs = cs->next;
    }
    return 0;
}

static long hvcs_register_present(void) {
    if (!hyperv_detect()) return 0;
    if (!hyperv_tsc_page()) return 0;
    struct clocksource *cs = clocksource_current();
    while (cs) {
        if (cs->name && hvcs_name_equals_hyperv_tsc(cs->name)) {
            if (cs->rating != HVCS_EXPECTED_RATING) return -2;
            if (cs->mult != 100 || cs->shift != 0)  return -3;
            if (cs->mask != CLOCKSOURCE_MASK_64)    return -4;
            return 0;
        }
        cs = cs->next;
    }
    return -1;
}

static long hvcs_monotonic(void) {
    if (!hyperv_detect() || !hyperv_tsc_page()) return 0;
    uint64_t a = hyperv_tsc_read_raw();
    uint64_t b = hyperv_tsc_read_raw();
    uint64_t c = hyperv_tsc_read_raw();
    if (b < a) return -1;
    if (c < b) return -2;
    return 0;
}

static long hvcs_raw_times_tick(void) {
    uint64_t raw = hyperv_tsc_read_raw();
    uint64_t ns  = hyperv_tsc_time_ns();
    return (ns == raw * 100ULL) ? 0 : -1;
}

/* ── KVM pvclock selftests (subcases 60-63) ─
 * On TCG / non-KVM hosts kvmclock_available()==0 and the register/monotonic
 * cases decay to no-op success (mirrors the Hyper-V pattern). */

#define KVMCS_EXPECTED_RATING  CLOCKSOURCE_RATING_KVM_PVCLOCK
#define KVMCS_FAKE_VERSION_OK  2u
#define KVMCS_FAKE_SYSTEM_TIME 1000000ULL
#define KVMCS_FAKE_TSC_TS      0ULL
#define KVMCS_FAKE_TSC_MUL     0x80000000u   /* (delta*mul)>>32 = delta/2 */
#define KVMCS_FAKE_TSC_SHIFT   1             /* then << 1 → identity */
#define KVMCS_FAKE_TSC_SAMPLE  1024ULL

static int kvmcs_name_equals(const char *s) {
    const char *want = "kvm-clock";
    while (*want) { if (*s++ != *want++) return 0; }
    return *s == '\0';
}

static long kvmcs_init_no_kvm(void) {
    if (kvmclock_available()) return 0;
    kvmclock_init();
    if (kvmclock_pvti() != 0) return -1;
    struct clocksource *cs = clocksource_current();
    while (cs) {
        if (cs->name && kvmcs_name_equals(cs->name)) return -2;
        cs = cs->next;
    }
    return 0;
}

static long kvmcs_register_present(void) {
    if (!kvmclock_available()) return 0;
    if (!kvmclock_pvti()) return -1;
    struct clocksource *cs = clocksource_current();
    while (cs) {
        if (cs->name && kvmcs_name_equals(cs->name)) {
            if (cs->rating != KVMCS_EXPECTED_RATING) return -2;
            if (cs->mult != 1 || cs->shift != 0)     return -3;
            if (cs->mask != CLOCKSOURCE_MASK_64)     return -4;
            return 0;
        }
        cs = cs->next;
    }
    return -5;
}

static long kvmcs_seqlock_retry(void) {
    struct pvclock_vcpu_time_info ti = {
        .version           = KVMCS_FAKE_VERSION_OK,
        .pad0              = 0,
        .tsc_timestamp     = KVMCS_FAKE_TSC_TS,
        .system_time       = KVMCS_FAKE_SYSTEM_TIME,
        .tsc_to_system_mul = KVMCS_FAKE_TSC_MUL,
        .tsc_shift         = KVMCS_FAKE_TSC_SHIFT,
        .flags             = PVCLOCK_TSC_STABLE_BIT,
        .pad               = { 0, 0 },
    };

    uint64_t ns_live = kvmclock_read_from(&ti);
    if (ns_live < KVMCS_FAKE_SYSTEM_TIME) return -1;

    uint64_t composed = kvmclock_compose_ns(&ti, KVMCS_FAKE_TSC_SAMPLE);
    uint64_t want = KVMCS_FAKE_SYSTEM_TIME + KVMCS_FAKE_TSC_SAMPLE;
    if (composed != want) return -2;

    ti.version = 1;  /* Odd → update-in-progress. */
    uint64_t odd_ver = ((const volatile struct pvclock_vcpu_time_info *)&ti)->version;
    if ((odd_ver & 1u) == 0) return -3;

    struct pvclock_vcpu_time_info neg = ti;
    neg.version           = KVMCS_FAKE_VERSION_OK;
    neg.tsc_shift         = -1;
    neg.tsc_to_system_mul = 0x80000000u;
    uint64_t shifted = kvmclock_compose_ns(&neg, 4);
    if (shifted != KVMCS_FAKE_SYSTEM_TIME + 1ULL) return -4;

    return 0;
}

static long kvmcs_monotonic(void) {
    if (!kvmclock_available() || !kvmclock_pvti()) return 0;
    uint64_t a = kvmclock_read_ns();
    uint64_t b = kvmclock_read_ns();
    uint64_t c = kvmclock_read_ns();
    if (b < a) return -1;
    if (c < b) return -2;
    return 0;
}

/* ── Hyper-V STimer selftests (subcases 70-74) ─
 * Direct-Mode-Probe und CONFIG-Builder laufen rein CPU-lokal — getestet auch
 * ohne Hyper-V. Register/Deadline-Cases degradieren zu no-op success wenn
 * hyperv_stimer_available()==0, analog zu TSC-Page/kvmclock. */

#define HVST_TEST_VECTOR      HV_STIMER0_DIRECT_VECTOR
#define HVST_TEST_SINT        HV_STIMER_SINT
#define HVST_TEST_NOW_TICKS   0x100000ULL
#define HVST_TEST_DELTA_NS    1000000ULL          /* 1 ms */
#define HVST_TEST_DELTA_TICKS 10000ULL            /* 1 ms / 100 ns */
#define HVST_SUB_PAST         0ULL                /* forces min-delta clamp */

static int hvst_name_equals(const char *s) {
    const char *want = "hv-stimer";
    while (*want) { if (*s++ != *want++) return 0; }
    return *s == '\0';
}

static long hvst_init_no_hyperv(void) {
    if (hyperv_detect()) return 0;
    hyperv_stimer_init();
    struct clock_event_device *ce = clock_event_current();
    while (ce) {
        if (ce->name && hvst_name_equals(ce->name)) return -1;
        ce = ce->next;
    }
    return 0;
}

static long hvst_config_build(void) {
    uint64_t direct = hyperv_stimer_build_config_direct(HVST_TEST_VECTOR, 0);
    if (!(direct & HV_STIMER_CONFIG_ENABLE))       return -1;
    if (!(direct & HV_STIMER_CONFIG_DIRECT_MODE))  return -2;
    if (direct & HV_STIMER_CONFIG_PERIODIC)        return -3;
    uint64_t vec = (direct & HV_STIMER_CONFIG_VECTOR_MASK)
                   >> HV_STIMER_CONFIG_VECTOR_SHIFT;
    if (vec != HVST_TEST_VECTOR)                   return -4;

    uint64_t direct_per = hyperv_stimer_build_config_direct(HVST_TEST_VECTOR, 1);
    if (!(direct_per & HV_STIMER_CONFIG_PERIODIC)) return -5;

    uint64_t synic = hyperv_stimer_build_config_synic(HVST_TEST_SINT, 0);
    if (!(synic & HV_STIMER_CONFIG_ENABLE))        return -6;
    if (synic & HV_STIMER_CONFIG_DIRECT_MODE)      return -7;
    uint64_t sintx = (synic & HV_STIMER_CONFIG_SINTX_MASK)
                     >> HV_STIMER_CONFIG_SINTX_SHIFT;
    if (sintx != HVST_TEST_SINT)                   return -8;

    return 0;
}

static long hvst_deadline_convert(void) {
    uint64_t want = HVST_TEST_NOW_TICKS + HVST_TEST_DELTA_TICKS;
    uint64_t got  = hyperv_stimer_compute_deadline(HVST_TEST_NOW_TICKS,
                                                   HVST_TEST_DELTA_NS);
    if (got != want) return -1;

    /* delta == 0 / past deadline must clamp to now + MIN_DELTA. */
    uint64_t clamped = hyperv_stimer_compute_deadline(HVST_TEST_NOW_TICKS,
                                                      HVST_SUB_PAST);
    if (clamped != HVST_TEST_NOW_TICKS + HV_STIMER_MIN_DELTA_TICKS) return -2;

    /* delta below min still clamps. */
    uint64_t tiny = hyperv_stimer_compute_deadline(HVST_TEST_NOW_TICKS,
                                                   HV_STIMER_TICK_NS);
    if (tiny != HVST_TEST_NOW_TICKS + HV_STIMER_MIN_DELTA_TICKS) return -3;

    return 0;
}

static long hvst_register_present(void) {
    if (!hyperv_stimer_available()) return 0;
    struct clock_event_device *ce = clock_event_current();
    while (ce) {
        if (ce->name && hvst_name_equals(ce->name)) {
            if (ce->rating != CLOCKSOURCE_RATING_HYPERV_TSC) return -2;
            if (!(ce->features & CLOCK_EVT_FEAT_ONESHOT))    return -3;
            if (!ce->set_next_event)                         return -4;
            if (!ce->shutdown)                               return -5;
            return 0;
        }
        ce = ce->next;
    }
    return -1;
}

static long hvst_shutdown_idempotent(void) {
    if (!hyperv_stimer_available()) return 0;
    struct clock_event_device *ce = clock_event_current();
    while (ce) {
        if (ce->name && hvst_name_equals(ce->name)) {
            if (ce->shutdown() != 0) return -1;
            if (ce->shutdown() != 0) return -2;
            return 0;
        }
        ce = ce->next;
    }
    return -3;
}

/* ── virtio-rtc probe selftests (subcases 90-93) ─
 * Probe-only driver (src/drivers/virtio/virtio_rtc.c). Tests verify the
 * spec-mandated device/request constants, that init() is idempotent, that
 * the boot probe picked a consistent (available,bus,slot) tuple, and that
 * the driver stays out of the clocksource chain — the deliberate non-goal
 * of Phase 7.7.8 (virtqueue-round-trip reads are unfit for the hot path). */

#define VRTC_PCI_DEVICE_ID         0x105Du
#define VRTC_VIRTIO_DEV_ID         29u
#define VRTC_REQ_READ              0x0001u
#define VRTC_REQ_READ_CROSS        0x0002u
#define VRTC_CLOCK_UTC             0u
#define VRTC_CLOCK_TAI             1u
#define VRTC_CLOCK_MONOTONIC       2u
#define VRTC_NAME_LEN              10

extern int  virtio_rtc_init(void);
extern int  virtio_rtc_available(void);
extern void virtio_rtc_location(int *bus_out, int *slot_out);

static int vrtc_name_equals(const char *s) {
    const char *want = "virtio-rtc";
    for (int i = 0; i < VRTC_NAME_LEN; i++) {
        if (s[i] != want[i]) return 0;
    }
    return s[VRTC_NAME_LEN] == '\0';
}

static long vrtc_spec_constants(void) {
    if (VRTC_PCI_DEVICE_ID != (0x1040u + VRTC_VIRTIO_DEV_ID)) return -1;
    if (VRTC_REQ_READ == VRTC_REQ_READ_CROSS)                return -2;
    if (VRTC_CLOCK_UTC == VRTC_CLOCK_MONOTONIC)              return -3;
    if (VRTC_CLOCK_TAI == VRTC_CLOCK_MONOTONIC)              return -4;
    return 0;
}

static long vrtc_probe_idempotent(void) {
    int r1 = virtio_rtc_init();
    int r2 = virtio_rtc_init();
    if (r1 != r2) return -1;
    int avail1 = virtio_rtc_available();
    int avail2 = virtio_rtc_available();
    if (avail1 != avail2)                     return -2;
    if (avail1 && r1 != 0)                    return -3;
    if (!avail1 && r1 == 0)                   return -4;
    return 0;
}

static long vrtc_location_consistency(void) {
    int bus = -2, slot = -2;
    virtio_rtc_location(&bus, &slot);
    if (virtio_rtc_available()) {
        if (bus  < 0 || bus  > 255) return -1;
        if (slot < 0 || slot > 31)  return -2;
    } else {
        if (bus  != -1) return -3;
        if (slot != -1) return -4;
    }
    return 0;
}

static long vrtc_not_in_clocksource_chain(void) {
    struct clocksource *cs = clocksource_current();
    while (cs) {
        if (cs->name && vrtc_name_equals(cs->name)) return -1;
        cs = cs->next;
    }
    return 0;
}

/* ── TSC calibration selftests (subcases 110-115) ─────
 * Exercise tsc_check_invariant via tsc_override_invariant_for_test + the
 * re-calibration helper. HPET path is verified against the HPET override
 * hook so no live HPET is required. Live CPUID is sampled in 110 + 114. */

#define TSC_HZ_MIN              500000000ULL
#define TSC_HZ_MAX              5000000000ULL
#define TSC_HPET_TEST_PERIOD_FS 10000000u
#define TSC_MATCH_TOLERANCE_PPM 200000           /* 20 % — TCG-Jitter */

static uint64_t tsc_test_mmio[HPET_TEST_BUF_WORDS] __attribute__((aligned(16)));

static void tsc_test_reset_hpet_buf(uint32_t period_fs, int num_comp, int is_64bit) {
    for (int i = 0; i < HPET_TEST_BUF_WORDS; i++) tsc_test_mmio[i] = 0;
    uint64_t caps =
        ((uint64_t)period_fs << HPET_GCAP_PERIOD_SHIFT) |
        ((uint64_t)((num_comp - 1) & HPET_GCAP_NUM_TIM_MASK)
             << HPET_GCAP_NUM_TIM_SHIFT) |
        (is_64bit ? HPET_GCAP_COUNT_SIZE : 0);
    tsc_test_mmio[HPET_REG_GCAP_ID / 8] = caps;
}

static uint64_t tsc_abs_diff(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

static long tsc_within_ppm(uint64_t got, uint64_t expect, uint64_t ppm) {
    uint64_t diff = tsc_abs_diff(got, expect);
    return (diff * 1000000ULL) <= (expect * ppm);
}

static long tsc_invariant_query(void) {
    tsc_override_invariant_for_test(-1);
    tsc_recalibrate_for_test();
    int v = tsc_is_invariant();
    return (v == 0 || v == 1) ? 0 : -1;
}

static long tsc_hpet_freq_match(void) {
    if (!hpet_available()) return 0;
    uint64_t ref = tsc_calibrated_hz();
    if (ref < TSC_HZ_MIN || ref > TSC_HZ_MAX) return -1;

    tsc_recalibrate_for_test();
    if (!tsc_calibrated_via_hpet()) return -2;
    uint64_t got = tsc_calibrated_hz();
    if (!tsc_within_ppm(got, ref, TSC_MATCH_TOLERANCE_PPM)) return -3;
    return 0;
}

static long tsc_pit_fallback(void) {
    /* Isolierter HPET-Pfad mit nicht-tickendem MAIN_CNT muss 0 liefern;
     * timer_init() wuerde dann auf PIT fallen. Der Fake-HPET ruft keinen
     * 10-ms PIT-Busy-Wait aus, sondern prueft nur die Timeout-Semantik. */
    tsc_test_reset_hpet_buf(TSC_HPET_TEST_PERIOD_FS, 1, 1);
    hpet_override_base_for_test((uint64_t)(uintptr_t)tsc_test_mmio,
                                TSC_HPET_TEST_PERIOD_FS, 1, 1);

    tsc_test_mmio[HPET_REG_MAIN_CNT / 8] = 0;
    uint64_t hz = tsc_calibrate_via_hpet_for_test();
    hpet_clear_override_for_test();
    return (hz == 0) ? 0 : -1;
}

static long tsc_rating_downgrade(void) {
    tsc_override_invariant_for_test(0);
    tsc_recalibrate_for_test();
    struct clocksource *cs = clocksource_current();
    long rc = 0;
    while (cs) {
        if (cs->name && cs->name[0] == 't' && cs->name[1] == 's' && cs->name[2] == 'c'
            && cs->name[3] == '\0') {
            if (cs->rating != CLOCKSOURCE_RATING_TSC_UNSTABLE) rc = -1;
            break;
        }
        cs = cs->next;
    }
    if (!rc && tsc_is_invariant() != 0) rc = -2;

    tsc_override_invariant_for_test(1);
    tsc_recalibrate_for_test();
    cs = clocksource_current();
    while (cs) {
        if (cs->name && cs->name[0] == 't' && cs->name[1] == 's' && cs->name[2] == 'c'
            && cs->name[3] == '\0') {
            if (!rc && cs->rating != CLOCKSOURCE_RATING_TSC) rc = -3;
            break;
        }
        cs = cs->next;
    }
    if (!rc && tsc_is_invariant() != 1) rc = -4;

    tsc_override_invariant_for_test(-1);
    tsc_recalibrate_for_test();
    return rc;
}

static long tsc_freq_plausible(void) {
    uint64_t hz = tsc_calibrated_hz();
    if (hz < TSC_HZ_MIN) return -1;
    if (hz > TSC_HZ_MAX) return -2;
    if (timer_tsc_per_ms == 0) return -3;
    uint64_t derived = timer_tsc_per_ms * 1000ULL;
    if (!tsc_within_ppm(derived, hz, 10000ULL)) return -4;
    return 0;
}

static long tsc_path_consistent(void) {
    int path_hpet = tsc_calibrated_via_hpet();
    int hpet_live = hpet_available();
    if (hpet_live && !path_hpet) return -1;
    if (!hpet_live && path_hpet) return -2;
    return 0;
}

/* ── Waitqueue/wake selftests (subcases 200-203) ─────
 * Exercise try_to_wake_up's mask filter, wait_queue_entry func dispatch,
 * signal_wake_up wrapper, and autoremove_wake_function return contract.
 *
 * Test threads are slab-allocated via thread_alloc with proc=NULL, so
 * schedule()'s !next->proc skip-path silently drops them if the run queue
 * picks them up before cleanup. State is moved to THREAD_DEAD before
 * thread_free so the picker's THREAD_DEAD branch reaps them via the
 * existing thread_free(next) path. */

static int wq_test_func_calls;
static unsigned int wq_test_func_last_mask;
static int wq_test_func(wait_queue_entry_t *e, unsigned int mask) {
    (void)e;
    wq_test_func_calls++;
    wq_test_func_last_mask = mask;
    return 1;
}

static void wq_test_thread_release(thread_t *t) {
    if (!t) return;
    __atomic_store_n(&t->state, THREAD_DEAD, __ATOMIC_RELEASE);
    thread_free(t);
}

static long wq_mask_filter_test(void) {
    thread_t *t = thread_alloc();
    if (!t) return -1;
    long rc = 0;

    /* (a) STOPPED + TASK_INTERRUPTIBLE-only mask -> No-Op (return 0). */
    __atomic_store_n(&t->state, THREAD_STOPPED, __ATOMIC_RELEASE);
    if (try_to_wake_up(t, TASK_INTERRUPTIBLE) != 0) { rc = -10; goto out; }
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_STOPPED) { rc = -11; goto out; }

    /* (b) STOPPED + TASK_STOPPED mask -> wakes (return 1, state=RUNNABLE). */
    if (try_to_wake_up(t, TASK_STOPPED) != 1) { rc = -20; goto out; }
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_RUNNABLE) { rc = -21; goto out; }

    /* (c) BLOCKED + TASK_STOPPED-only mask -> No-Op. */
    __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
    if (try_to_wake_up(t, TASK_STOPPED) != 0) { rc = -30; goto out; }
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) { rc = -31; goto out; }

    /* (d) BLOCKED + TASK_INTERRUPTIBLE -> wakes. */
    if (try_to_wake_up(t, TASK_INTERRUPTIBLE) != 1) { rc = -40; goto out; }
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_RUNNABLE) { rc = -41; goto out; }

    /* (e) Already RUNNABLE -> No-Op for any mask. */
    if (try_to_wake_up(t, TASK_NORMAL) != 0) { rc = -50; goto out; }

out:
    wq_test_thread_release(t);
    return rc;
}

static long wq_func_called_test(void) {
    thread_t *t = thread_alloc();
    if (!t) return -1;
    __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);

    wait_queue_head_t wq;
    init_waitqueue_head(&wq);
    wait_queue_entry_t ent = {
        .task = t, .flags = 0, .func = wq_test_func, .next = 0, .prev = 0,
    };
    add_wait_queue(&wq, &ent);

    wq_test_func_calls = 0;
    wq_test_func_last_mask = 0;
    int woken = wake_up(&wq);

    long rc = 0;
    if (wq_test_func_calls != 1) rc = -1;
    else if (woken != 1) rc = -2;
    else if (wq_test_func_last_mask != TASK_NORMAL) rc = -3;
    /* Custom func did NOT actually CAS state — entry's task remains BLOCKED. */
    else if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) rc = -4;

    /* Test wake_up_interruptible passes TASK_INTERRUPTIBLE. */
    wq_test_func_calls = 0;
    wq_test_func_last_mask = 0;
    woken = wake_up_interruptible(&wq);
    if (rc == 0) {
        if (wq_test_func_calls != 1) rc = -5;
        else if (woken != 1) rc = -6;
        else if (wq_test_func_last_mask != TASK_INTERRUPTIBLE) rc = -7;
    }

    remove_wait_queue(&wq, &ent);
    wq_test_thread_release(t);
    return rc;
}

static long wq_signal_wake_up_test(void) {
    thread_t *t = thread_alloc();
    if (!t) return -1;
    long rc = 0;

    /* signal_wake_up matches BLOCKED via TASK_INTERRUPTIBLE | TASK_KILLABLE. */
    __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
    signal_wake_up(t);
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_RUNNABLE) { rc = -1; goto out; }

    /* signal_wake_up does NOT touch STOPPED (that's SIGCONT's job). */
    __atomic_store_n(&t->state, THREAD_STOPPED, __ATOMIC_RELEASE);
    signal_wake_up(t);
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_STOPPED) { rc = -2; goto out; }

out:
    wq_test_thread_release(t);
    return rc;
}

/* sched_add idempotency must hold even when t->rq_next is a stale non-NULL
 * pointer (use-after-free of a predecessor in the singly-linked queue). The
 * old check `if (rq[prio].tail == t || t->rq_next) return;` mistook stale
 * pointers for membership and refused to enqueue, dropping wakeups silently.
 *
 * Reproduce: arrange t with state=RUNNABLE and rq_next pointing into an
 * unrelated address (any non-NULL value), call sched_add(t), then verify a
 * subsequent sched_pick returns t. With the old check sched_pick returns 0
 * (because t was never enqueued); with the list-walk check sched_pick
 * returns t. */
extern void sched_add(thread_t *t);
extern thread_t *sched_pick(void);

extern int sched_test_stale_rq_next(thread_t *t);

static long sched_stale_rq_next_idempotency(void) {
    thread_t *t = thread_alloc();
    if (!t) return -1;
    long rc = 0;

    t->priority = 0;
    int picked_ok = sched_test_stale_rq_next(t);
    if (picked_ok != 0) rc = picked_ok;

    wq_test_thread_release(t);
    return rc;
}

/* sched_dequeue must remove t from any prio-queue it sits on, leaving the
 * remaining queue intact. Build a 3-element queue [a, b, c], dequeue the
 * middle (b), verify pick order is a then c.
 *
 * Use sched_test_drain_3 so insert/dequeue/pick happen under one rq_lock
 * acquisition: peer-CPU schedule()'s pick-loop would otherwise consume
 * our synthetic threads (proc==NULL) and break the pick-order invariant. */
extern int sched_test_drain_3(thread_t *a, thread_t *b, thread_t *c,
                              thread_t **p1, thread_t **p2, thread_t **p3,
                              int dequeue_b);

static long sched_dequeue_middle(void) {
    thread_t *a = thread_alloc();
    thread_t *b = thread_alloc();
    thread_t *c = thread_alloc();
    if (!a || !b || !c) {
        if (a) wq_test_thread_release(a);
        if (b) wq_test_thread_release(b);
        if (c) wq_test_thread_release(c);
        return -1;
    }
    long rc = 0;

    a->priority = 0; b->priority = 0; c->priority = 0;

    thread_t *p1 = 0, *p2 = 0, *p3 = 0;
    int b_rqnext_after_dequeue = sched_test_drain_3(a, b, c, &p1, &p2, &p3, 1);

    if (b_rqnext_after_dequeue != 0) { rc = -10; goto out; }
    if (p1 != a) { rc = -20; goto out; }
    if (p2 != c) { rc = -21; goto out; }
    if (p3 != 0) { rc = -22; goto out; }

out:
    wq_test_thread_release(a);
    wq_test_thread_release(b);
    wq_test_thread_release(c);
    return rc;
}

static long wq_autoremove_test(void) {
    thread_t *t = thread_alloc();
    if (!t) return -1;
    __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);

    wait_queue_head_t wq;
    init_waitqueue_head(&wq);
    wait_queue_entry_t ent = {
        .task = t, .flags = 0, .func = autoremove_wake_function,
        .next = 0, .prev = 0,
    };
    add_wait_queue(&wq, &ent);

    long rc = 0;
    /* autoremove returns 1 on successful wake. */
    if (autoremove_wake_function(&ent, TASK_INTERRUPTIBLE) != 1) { rc = -1; goto out; }
    /* AUTOREMOVE flag set so finish_wait knows to dequeue. */
    if (!(ent.flags & WQ_FLAG_AUTOREMOVE)) { rc = -2; goto out; }
    /* State transitioned to RUNNABLE. */
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_RUNNABLE) { rc = -3; goto out; }

    /* finish_wait dequeues the entry; head must be empty afterwards. */
    finish_wait(&wq, &ent);
    if (wq.head != 0) { rc = -4; goto out; }
    if (ent.next != 0 || ent.prev != 0) { rc = -5; goto out; }

    /* No-match path: thread already RUNNABLE -> autoremove returns 0,
     * AUTOREMOVE flag NOT set on this call (we cleared it via finish_wait). */
    ent.flags = 0;
    if (autoremove_wake_function(&ent, TASK_INTERRUPTIBLE) != 0) { rc = -6; goto out; }
    if (ent.flags & WQ_FLAG_AUTOREMOVE) { rc = -7; goto out; }

out:
    wq_test_thread_release(t);
    return rc;
}

/* ── RT Query (subcommands via a1) ────────────────── */

static long do_cosmo_rt_query(long a1, long a2, long a3, long a4) {
    switch (a1) {
    case 3: {
        extern int timer_wheel_add(uint8_t action, void *ctx, uint32_t timeout_ms);
        return (long)timer_wheel_add((uint8_t)a2, (void *)a3, (uint32_t)a4);
    }
    case 4: {
        extern int timer_wheel_cancel(void *ctx);
        return (long)timer_wheel_cancel((void *)a2);
    }
    case 5: {
        extern int timer_wheel_active_count(void);
        return (long)timer_wheel_active_count();
    }
    case 10: return cst_rating_sort();
    case 11: return cst_register_higher();
    case 12: return cst_register_lower();
    case 13: return cst_unregister_current();
    case 14: return cst_unregister_noncurrent();
    case 15: return cst_mult_shift_pm();
    case 16: return cst_mult_shift_tsc();
    case 17: return cst_mask_wrap();
    case 18: return cst_monotonic();
    case 19: return cst_clock_event();
    case 20: return at_rsdp_validate();
    case 21: return at_rsdp_checksum();
    case 22: return at_revision_routing();
    case 23: return at_xsdt_walk_find();
    case 24: return at_xsdt_walk_missing();
    case 25: return at_fadt_pm_tmr();
    case 26: return at_hpet_fields();
    case 27: return at_madt_counts();
    case 28: return at_corrupt_sdt();
    case 29: return at_duplicate_sig();
    case 30: return at_checksum_helper();
    case 40: return ht_period_to_freq();
    case 41: return ht_read_counter();
    case 42: return ht_clocksource_mask();
    case 43: return ht_mult_shift_sanity();
    case 44: return ht_boot_probe();
    case 50: return hvcs_init_no_hyperv();
    case 51: return hvcs_register_present();
    case 52: return hvcs_monotonic();
    case 53: return hvcs_raw_times_tick();
    case 60: return kvmcs_init_no_kvm();
    case 61: return kvmcs_register_present();
    case 62: return kvmcs_seqlock_retry();
    case 63: return kvmcs_monotonic();
    case 80: return apm_missing_fadt();
    case 81: return apm_mask_24_vs_32();
    case 82: return apm_mult_shift_sanity();
    case 83: return apm_monotonic();
    case 84: return apm_boot_probe();
    case 70: return hvst_init_no_hyperv();
    case 71: return hvst_config_build();
    case 72: return hvst_deadline_convert();
    case 73: return hvst_register_present();
    case 74: return hvst_shutdown_idempotent();
    case 90: return vrtc_spec_constants();
    case 91: return vrtc_probe_idempotent();
    case 92: return vrtc_location_consistency();
    case 93: return vrtc_not_in_clocksource_chain();
    case 110: return tsc_invariant_query();
    case 111: return tsc_hpet_freq_match();
    case 112: return tsc_pit_fallback();
    case 113: return tsc_rating_downgrade();
    case 114: return tsc_freq_plausible();
    case 115: return tsc_path_consistent();
    case 200: return wq_mask_filter_test();
    case 201: return wq_func_called_test();
    case 202: return wq_signal_wake_up_test();
    case 203: return wq_autoremove_test();
    case 204: return sched_stale_rq_next_idempotency();
    case 205: return sched_dequeue_middle();
    default: return -EINVAL;
    }
}

/* ── CosmoRT Syscall Dispatcher (0x10000+) ────────── */

long cosmo_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a6;
    /* RT_QUERY is read-only observability — any process may call it */
    if (num == SYS_COSMO_RT_QUERY)
        return do_cosmo_rt_query(a1, a2, a3, a4);
    /* All other 0x10000+ syscalls are HW primitives — drivers only */
    process_t *p = proc_current();
    if (!p || !p->is_driver) return -EPERM;
    switch (num) {
    case SYS_COSMO_MMIO_MAP:     return do_cosmo_mmio_map(a1, a2, a3);
    case SYS_COSMO_DMA_ALLOC:    return do_cosmo_dma_alloc(a1, a2, a3);
    case SYS_COSMO_DMA_FREE:     return do_cosmo_dma_free(a1, a2);
    case SYS_COSMO_IRQ_REGISTER: return do_cosmo_irq_register(a1, a2, a3);
    case SYS_COSMO_PCI_READ:     return do_cosmo_pci_read(a1, a2, a3, a4, a5);
    case SYS_COSMO_PCI_WRITE:    return do_cosmo_pci_write(a1, a2, a3, a4, a5);
    case SYS_COSMO_FW_LOAD:      return do_cosmo_fw_load(a1, a2, a3);
    case SYS_COSMO_NIC_ATTACH:   return do_cosmo_nic_attach(a1);
    case SYS_COSMO_KEXEC:        return do_cosmo_kexec(a1, a2);
    default:                     return -ENOSYS;
    }
}
