/* CosmoRT — Hardware primitive syscalls (SYS_COSMO_*) for userspace drivers.
 * Separate dispatch from Linux syscalls for clean jump-table generation. */

#include "internal.h"
#include "core/clocksource.h"
#include "hw/acpi.h"
#include "config.h"
#include "memops.h"

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
 * via SYS_COSMO_RT_QUERY. Fake clocksources register with ratings above TSC
 * and are always unregistered before return so the live TSC stays current. */

#define CS_TEST_RATING_HIGH (CLOCKSOURCE_RATING_MAX + 1 - 50)
#define CS_TEST_RATING_MID  (CLOCKSOURCE_RATING_MAX + 1 - 100)
#define CS_TEST_RATING_LOW  (CLOCKSOURCE_RATING_MAX + 1 - 150)
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
    if (clock_event_current() != &ce) return -1;
    cst_ce_last_delta = 0;
    int r = clock_event_set_oneshot(123456ULL);
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
