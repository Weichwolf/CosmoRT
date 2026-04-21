/* CosmoRT Hyper-V Detection, Enlightenments, SynIC, Hypercall
 *
 * Kernel-side code: modifies MSRs, IDT vectors, allocates hypercall page.
 * On non-Hyper-V systems, hyperv_detect() returns 0 and nothing else runs.
 */

#include "hw/hyperv.h"
#include "cosmort.h"
#include "config.h"
#include "mm/page_alloc.h"
#include "mm/paging.h"
#include "core/irq.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "memops.h"
#include "arch/arch.h"

/* ---- MSR helpers ---- */

static inline uint64_t rdmsr(uint32_t msr) { return arch_rdmsr(msr); }
static inline void wrmsr(uint32_t msr, uint64_t val) { arch_wrmsr(msr, val); }

/* ---- State ---- */

static void *hc_page;                  /* hypercall page (kernel virtual) */
static uint64_t hc_phys;              /* hypercall page physical */
static struct hv_message *simp_virt;   /* SynIC Interrupt Message Page */
static uint64_t simp_phys;
static void *siefp_virt;              /* SynIC Event Flags Page */
static uint64_t siefp_phys;
static struct hv_tsc_page *tsc_page;
static spinlock_t hc_lock = SPINLOCK_INIT;

/* VMBus message/event handlers (set by vmbus_init) */
static void (*vmbus_msg_handler)(void);
static void (*vmbus_evt_handler)(void);

void hyperv_set_vmbus_msg_handler(void (*fn)(void)) { vmbus_msg_handler = fn; }
void hyperv_set_vmbus_evt_handler(void (*fn)(void)) { vmbus_evt_handler = fn; }

/* ---- Detection ---- */

int hyperv_detect(void) {
    uint32_t eax, ebx, ecx, edx;
    arch_cpuid(0x40000000, &eax, &ebx, &ecx, &edx);
    /* "Microsoft Hv" = ebx:ecx:edx = 7263694D:666F736F:76482074 */
    return (ebx == 0x7263694D && ecx == 0x666F736F && edx == 0x76482074);
}

/* ---- SynIC interrupt handlers ---- */

static void synic_msg_isr(int vector) {
    (void)vector;
    if (vmbus_msg_handler)
        vmbus_msg_handler();
    lapic_eoi();
}

static void synic_evt_isr(int vector) {
    (void)vector;
    if (vmbus_evt_handler)
        vmbus_evt_handler();
    lapic_eoi();
}

/* ---- Init ---- */

void hyperv_init(void) {
    /* 1. Guest OS ID (CosmoRT vendor = 0x0001, version 0.1) */
    wrmsr(HV_X64_MSR_GUEST_OS_ID, 0x0001000000010000ULL);

    /* 2. Hypercall page */
    if (cosmo_dma_alloc(4096, &hc_page, &hc_phys) < 0) {
        serial_puts("hyperv: hypercall page alloc failed\n");
        return;
    }
    wrmsr(HV_X64_MSR_HYPERCALL, hc_phys | 1);
    serial_puts("hyperv: hypercall page at ");
    serial_hex64(hc_phys);
    serial_putchar('\n');

    /* 3. Reference TSC page */
    void *tsc_virt;
    uint64_t tsc_phys_addr;
    if (cosmo_dma_alloc(4096, &tsc_virt, &tsc_phys_addr) == 0) {
        tsc_page = (struct hv_tsc_page *)tsc_virt;
        wrmsr(HV_X64_MSR_REFERENCE_TSC, tsc_phys_addr | 1);
        serial_puts("hyperv: TSC page at ");
        serial_hex64(tsc_phys_addr);
        serial_putchar('\n');
    }

    /* 4. Register SynIC ISR vectors in IDT */
    irq_register(HV_VMBUS_MSG_VECTOR, synic_msg_isr);
    irq_register(HV_VMBUS_EVT_VECTOR, synic_evt_isr);

    serial_puts("hyperv: init complete\n");
}

void hyperv_synic_init(void) {
    /* Allocate SIMP (SynIC Interrupt Message Page) */
    if (cosmo_dma_alloc(4096, (void **)&simp_virt, &simp_phys) < 0) {
        serial_puts("hyperv: SIMP alloc failed\n");
        return;
    }
    kmemset(simp_virt, 0, 4096);

    /* Allocate SIEFP (SynIC Event Flags Page) */
    if (cosmo_dma_alloc(4096, &siefp_virt, &siefp_phys) < 0) {
        serial_puts("hyperv: SIEFP alloc failed\n");
        return;
    }
    kmemset(siefp_virt, 0, 4096);

    /* Enable SynIC */
    wrmsr(HV_X64_MSR_SCONTROL, 1);

    /* Set SIMP and SIEFP */
    wrmsr(HV_X64_MSR_SIMP, simp_phys | 1);
    wrmsr(HV_X64_MSR_SIEFP, siefp_phys | 1);

    /* Configure SINTs:
     * SINT2 → vector 0x31 (VMBus messages), AutoEOI
     * SINT3 → vector 0x32 (VMBus events), AutoEOI
     * All others masked. */
    for (int i = 0; i < 16; i++) {
        if (i == HV_VMBUS_MSG_SINT) {
            wrmsr(HV_X64_MSR_SINT0 + i,
                  (uint64_t)HV_VMBUS_MSG_VECTOR | (1ULL << 17));  /* vector + AutoEOI */
        } else if (i == HV_VMBUS_EVT_SINT) {
            wrmsr(HV_X64_MSR_SINT0 + i,
                  (uint64_t)HV_VMBUS_EVT_VECTOR | (1ULL << 17));
        } else {
            wrmsr(HV_X64_MSR_SINT0 + i, (1ULL << 16));  /* masked */
        }
    }

    serial_puts("hyperv: SynIC enabled (SINT2→0x31, SINT3→0x32)\n");
}

/* ---- Hypercall ---- */

static uint64_t do_hypercall(uint64_t control, uint64_t input_phys, uint64_t output_phys) {
    return arch_hyperv_call(control, input_phys, output_phys, hc_page);
}

uint64_t hyperv_post_message(uint32_t conn_id, uint32_t msg_type,
                              const void *payload, size_t len) {
    if (!hc_page || len > 240) return (uint64_t)-1;

    /* Use a DMA-accessible buffer for the hypercall input */
    static struct hv_post_msg_input msg __attribute__((aligned(4096)));
    static uint64_t msg_phys_cached;

    uint64_t flags;
    spin_lock_irq(&hc_lock, &flags);

    if (!msg_phys_cached)
        msg_phys_cached = virt_to_phys(&msg);

    msg.connection_id = conn_id;
    msg.reserved = 0;
    msg.message_type = msg_type;
    msg.payload_size = (uint32_t)len;
    kmemset(msg.payload, 0, 240);
    if (len > 0)
        kmemcpy(msg.payload, payload, len);

    uint64_t control = HV_POST_MESSAGE;  /* simple call, no fast flag */
    uint64_t result = do_hypercall(control, msg_phys_cached, 0);

    spin_unlock_irq(&hc_lock, flags);
    return result;
}

uint64_t hyperv_signal_event(uint32_t conn_id) {
    if (!hc_page) return (uint64_t)-1;
    /* HvSignalEvent takes connection_id in input param */
    return do_hypercall(HV_SIGNAL_EVENT, conn_id, 0);
}

/* ---- Message receive ---- */

struct hv_message *hyperv_simp_slot(int sint) {
    if (!simp_virt || sint < 0 || sint > 15) return 0;
    return &simp_virt[sint];
}

int hyperv_msg_recv(int sint, void *buf, size_t bufsize) {
    struct hv_message *slot = hyperv_simp_slot(sint);
    if (!slot || slot->type == HV_MESSAGE_NONE)
        return 0;

    int sz = slot->payload_size;
    if (sz > (int)bufsize) sz = (int)bufsize;
    if (sz > 0 && buf)
        kmemcpy(buf, slot->payload, (size_t)sz);

    /* Clear the slot and signal EOM if more messages pending */
    int pending = slot->flags & 1;
    slot->type = HV_MESSAGE_NONE;
    arch_mfence();

    if (pending)
        wrmsr(HV_X64_MSR_EOM, 0);

    return sz;
}

/* ---- Reference TSC time ----
 *
 * Hyper-V TLFS v6.0b §12.7: counter ticks 100ns. sequence==0 signals that the
 * host is updating the page (or has not yet initialised it) — readers must
 * fall back. Otherwise read scale/offset, snapshot rdtsc, recheck sequence.
 */

#define HV_TSC_INVALID_SEQUENCE  0u
#define HV_TSC_TICK_NS           100u

uint64_t hyperv_tsc_read_raw(void) {
    if (!tsc_page) return 0;

    uint32_t seq;
    uint64_t tsc, scale;
    int64_t offset;

    do {
        seq = tsc_page->sequence;
        arch_mfence();
        if (seq == HV_TSC_INVALID_SEQUENCE) return 0;
        scale = tsc_page->tsc_scale;
        offset = tsc_page->tsc_offset;
        tsc = arch_rdtsc();
        arch_mfence();
    } while (tsc_page->sequence != seq);

    __uint128_t wide = (__uint128_t)tsc * scale;
    return (uint64_t)(wide >> 64) + (uint64_t)offset;
}

uint64_t hyperv_tsc_time_ns(void) {
    return hyperv_tsc_read_raw() * HV_TSC_TICK_NS;
}

struct hv_tsc_page *hyperv_tsc_page(void) { return tsc_page; }
