/* CosmoRT Hyper-V Detection + Enlightenments + SynIC
 *
 * Kernel-side: modifies MSRs, IDT, hypercall page.
 * Detects Hyper-V via CPUID, sets up SynIC for VMBus interrupts.
 */
#ifndef HYPERV_H
#define HYPERV_H

#include <stdint.h>
#include <stddef.h>

/* ---- MSR definitions ---- */

#define HV_X64_MSR_GUEST_OS_ID     0x40000000
#define HV_X64_MSR_HYPERCALL       0x40000001
#define HV_X64_MSR_VP_INDEX        0x40000002
#define HV_X64_MSR_REFERENCE_TSC   0x40000021
#define HV_X64_MSR_SCONTROL        0x40000080
#define HV_X64_MSR_SIEFP           0x40000082
#define HV_X64_MSR_SIMP            0x40000083
#define HV_X64_MSR_EOM             0x40000084
#define HV_X64_MSR_SINT0           0x40000090
#define HV_X64_MSR_STIMER0_CONFIG  0x400000B0
#define HV_X64_MSR_STIMER0_COUNT   0x400000B1
#define HV_X64_MSR_STIMER1_CONFIG  0x400000B2
#define HV_X64_MSR_STIMER1_COUNT   0x400000B3
#define HV_X64_MSR_STIMER2_CONFIG  0x400000B4
#define HV_X64_MSR_STIMER2_COUNT   0x400000B5
#define HV_X64_MSR_STIMER3_CONFIG  0x400000B6
#define HV_X64_MSR_STIMER3_COUNT   0x400000B7
#define HV_X64_MSR_EOI             0x40000070

/* STIMER CONFIG layout (TLFS v6.0b §15.3):
 *   [0]     ENABLE
 *   [1]     PERIODIC
 *   [2]     LAZY          (skip expiration while vCPU not running)
 *   [3]     AUTO_ENABLE   (re-arm on expiry without host ack)
 *   [16:19] SINTX         (SynIC interrupt index, used when bit 12 == 0)
 *   [12]    DIRECT_MODE   (fire as IDT vector instead of SynIC message)
 *   [20:27] DIRECT_VECTOR (IDT vector when bit 12 == 1)
 */
#define HV_STIMER_CONFIG_ENABLE        (1ULL << 0)
#define HV_STIMER_CONFIG_PERIODIC      (1ULL << 1)
#define HV_STIMER_CONFIG_LAZY          (1ULL << 2)
#define HV_STIMER_CONFIG_AUTO_ENABLE   (1ULL << 3)
#define HV_STIMER_CONFIG_DIRECT_MODE   (1ULL << 12)
#define HV_STIMER_CONFIG_SINTX_SHIFT   16
#define HV_STIMER_CONFIG_SINTX_MASK    (0xFULL << HV_STIMER_CONFIG_SINTX_SHIFT)
#define HV_STIMER_CONFIG_VECTOR_SHIFT  20
#define HV_STIMER_CONFIG_VECTOR_MASK   (0xFFULL << HV_STIMER_CONFIG_VECTOR_SHIFT)

/* Hyper-V feature CPUID leaves (TLFS §2.4). */
#define HV_CPUID_FEATURES               0x40000003u
#define HV_FEATURE_SYNIC_MSRS_BIT       2        /* EAX bit 2 */
#define HV_FEATURE_STIMER_MSRS_BIT      3        /* EAX bit 3 */
#define HV_FEATURE_STIMER_DIRECT_BIT    19       /* EDX bit 19 */

/* Hypercall codes */
#define HV_POST_MESSAGE            0x005C
#define HV_SIGNAL_EVENT            0x005D

/* SynIC vectors: 0x31 for VMBus messages, 0x32 for VMBus events */
#define HV_VMBUS_MSG_SINT          2
#define HV_VMBUS_EVT_SINT          3
#define HV_VMBUS_MSG_VECTOR        0x31
#define HV_VMBUS_EVT_VECTOR        0x32

/* Message types */
#define HV_MESSAGE_NONE            0x00000000
#define HV_MESSAGE_VMBUS           0x00000001

/* SynIC message slot (256 bytes each, 16 slots per SIMP page) */
struct hv_message {
    uint32_t type;
    uint8_t  payload_size;
    uint8_t  flags;          /* bit 0 = message pending */
    uint16_t reserved;
    union {
        uint64_t sender;
        struct {
            uint32_t connection_id;
            uint32_t rsvd;
        };
    };
    uint8_t  payload[240];
} __attribute__((packed));

/* Hypercall input for HvPostMessage */
struct hv_post_msg_input {
    uint32_t connection_id;
    uint32_t reserved;
    uint32_t message_type;
    uint32_t payload_size;
    uint8_t  payload[240];
} __attribute__((packed));

/* Reference TSC page (for high-res time without VMEXIT) */
struct hv_tsc_page {
    volatile uint32_t sequence;
    uint32_t reserved1;
    uint64_t tsc_scale;
    int64_t  tsc_offset;
    uint64_t reserved2[509];
} __attribute__((packed));

/* ---- API ---- */

/* Detect Hyper-V via CPUID. Returns 1 if running on Hyper-V, 0 otherwise. */
int hyperv_detect(void);

/* Initialize: Guest OS ID, Hypercall Page, Reference TSC. */
void hyperv_init(void);

/* Setup SynIC on current CPU: SIMP, SIEFP, SINT entries. */
void hyperv_synic_init(void);

/* Post a message to the host via hypercall page. */
uint64_t hyperv_post_message(uint32_t conn_id, uint32_t msg_type,
                              const void *payload, size_t len);

/* Signal an event connection. */
uint64_t hyperv_signal_event(uint32_t conn_id);

/* Read a pending SynIC message for a given SINT. Returns payload size, 0 if none. */
int hyperv_msg_recv(int sint, void *buf, size_t bufsize);

/* Get pointer to SIMP page (for direct message access). */
struct hv_message *hyperv_simp_slot(int sint);

/* High-resolution time from Reference TSC (ns since boot). */
uint64_t hyperv_tsc_time_ns(void);

/* Raw Reference-TSC-Page counter in 100ns units. Uses the sequence lock to
 * coordinate with host-side updates; returns 0 while the page is invalid
 * (sequence == 0). */
uint64_t hyperv_tsc_read_raw(void);

/* Pointer to the mapped Reference TSC page (NULL if not allocated).
 * Exposed so the clocksource wrapper can decide whether to register. */
struct hv_tsc_page *hyperv_tsc_page(void);

/* Register the Hyper-V Reference-TSC clocksource with the core. No-op when
 * Hyper-V is not present or the TSC page is unavailable. */
void hyperv_clocksource_init(void);

/* Hyper-V Synthetic Timer (STIMER0) as clock_event_device rating 400.
 * No-op unless Hyper-V is present, the TSC page is mapped, and CPUID reports
 * Synthetic Timers. Direct-Mode is used when available (simpler dispatch via
 * IDT vector), otherwise falls back to SynIC message via a dedicated SINT. */
void hyperv_stimer_init(void);

/* Predicates for tests. Probe the current CPU without side effects. */
int  hyperv_stimer_available(void);
int  hyperv_stimer_direct_mode_available(void);

/* Pure helpers for tests — construct the CONFIG-MSR payload without touching
 * hardware. stimer_build_config_direct() sets DIRECT_MODE | VECTOR<<20.
 * stimer_build_config_synic() sets SINTX<<16 instead. Both always set
 * ENABLE; periodic!=0 adds PERIODIC. */
uint64_t hyperv_stimer_build_config_direct(uint8_t vector, int periodic);
uint64_t hyperv_stimer_build_config_synic(uint8_t sint, int periodic);

/* Convert a ns delta to an absolute STIMER COUNT value relative to the given
 * "now" in 100ns ticks, clamped by HV_STIMER_MIN_DELTA_TICKS so a caller can
 * never arm a deadline in the past. Exposed for tests. */
uint64_t hyperv_stimer_compute_deadline(uint64_t now_ticks, uint64_t delta_ns);

/* Minimum forward distance enforced on every set_next_event, in 100ns ticks.
 * 1000 ticks = 100us — large enough to avoid an interrupt storm if a caller
 * passes delta_ns == 0, small enough to stay inside one tick period. */
#define HV_STIMER_MIN_DELTA_TICKS  1000u
#define HV_STIMER_TICK_NS          100u

/* IDT vector assigned to STIMER0 in direct mode. Picked above the SynIC
 * message (0x31) and event (0x32) vectors and clear of all current IRQs. */
#define HV_STIMER0_DIRECT_VECTOR   0x33

/* Dedicated SynIC SINT for the STIMER0 fallback path. SINT2/3 are already
 * claimed by VMBus message/event; use SINT4. Mapped to IDT vector 0x34. */
#define HV_STIMER_SINT             4
#define HV_STIMER_SINT_VECTOR      0x34

#endif
