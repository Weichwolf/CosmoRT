/* CosmoRT — Hardware primitive syscalls (SYS_COSMO_*) for userspace drivers.
 * Separate dispatch from Linux syscalls for clean jump-table generation. */

#include "internal.h"
#include "core/clocksource.h"

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
