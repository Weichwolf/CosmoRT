/* CosmoRT Hyper-V Detection + Enlightenments + SynIC */
#ifndef HYPERV_H
#define HYPERV_H

#include <stdint.h>
#include <stddef.h>

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
#define HV_X64_MSR_EOI             0x40000070

#define HV_POST_MESSAGE            0x005C
#define HV_SIGNAL_EVENT            0x005D

#define HV_VMBUS_MSG_SINT          2
#define HV_VMBUS_EVT_SINT          3
#define HV_VMBUS_MSG_VECTOR        0x31
#define HV_VMBUS_EVT_VECTOR        0x32

#define HV_MESSAGE_NONE            0x00000000
#define HV_MESSAGE_VMBUS           0x00000001

struct hv_message {
    uint32_t type;
    uint8_t  payload_size;
    uint8_t  flags;
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

struct hv_post_msg_input {
    uint32_t connection_id;
    uint32_t reserved;
    uint32_t message_type;
    uint32_t payload_size;
    uint8_t  payload[240];
} __attribute__((packed));

struct hv_tsc_page {
    volatile uint32_t sequence;
    uint32_t reserved1;
    uint64_t tsc_scale;
    int64_t  tsc_offset;
    uint64_t reserved2[509];
} __attribute__((packed));

int hyperv_detect(void);

void hyperv_init(void);

void hyperv_synic_init(void);

uint64_t hyperv_post_message(uint32_t conn_id, uint32_t msg_type,
                              const void *payload, size_t len);

uint64_t hyperv_signal_event(uint32_t conn_id);

int hyperv_msg_recv(int sint, void *buf, size_t bufsize);

struct hv_message *hyperv_simp_slot(int sint);

uint64_t hyperv_tsc_time_ns(void);

#endif
