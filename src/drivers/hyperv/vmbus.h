/* CosmoRT VMBus Transport — channel management, ring buffers, signaling */
#ifndef VMBUS_H
#define VMBUS_H

#include <stdint.h>
#include <stddef.h>

#define CHANNELMSG_INVALID              0
#define CHANNELMSG_OFFERCHANNEL         1
#define CHANNELMSG_RESCIND_CHANNELOFFER 2
#define CHANNELMSG_REQUESTOFFERS        3
#define CHANNELMSG_ALLOFFERS_DELIVERED  4
#define CHANNELMSG_OPENCHANNEL          5
#define CHANNELMSG_OPENCHANNEL_RESULT   6
#define CHANNELMSG_CLOSECHANNEL         7
#define CHANNELMSG_GPADL_HEADER         8
#define CHANNELMSG_GPADL_BODY           9
#define CHANNELMSG_GPADL_CREATED        10
#define CHANNELMSG_GPADL_TEARDOWN       11
#define CHANNELMSG_GPADL_TORNDOWN       12
#define CHANNELMSG_RELID_RELEASED       13
#define CHANNELMSG_INITIATE_CONTACT     14
#define CHANNELMSG_VERSION_RESPONSE     15
#define CHANNELMSG_UNLOAD               16
#define CHANNELMSG_UNLOAD_RESPONSE      17

#define VMBUS_VERSION_WIN10            ((5 << 16) | 0)
#define VMBUS_VERSION_WIN8_1           ((4 << 16) | 0)
#define VMBUS_VERSION_WIN8             ((3 << 16) | 0)

#define VMBUS_MESSAGE_CONNECTION_ID    1

#define VMBUS_MAX_CHANNELS             64

typedef struct {
    uint8_t data[16];
} vmbus_guid_t;

struct vmbus_ring_hdr {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t interrupt_mask;
    volatile uint32_t pending_send_size;
    uint32_t reserved[12];
};

#define VMBUS_RING_HDR_SIZE 64

struct vmbus_channel {
    uint32_t child_relid;
    vmbus_guid_t type_guid;
    vmbus_guid_t instance_guid;
    uint32_t monitor_id;
    int      state;

    void    *ring_mem;
    uint64_t ring_phys;
    uint32_t ring_size;
    uint32_t gpadl_handle;

    struct vmbus_ring_hdr *tx_ring;
    uint8_t               *tx_data;
    uint32_t               tx_size;
    struct vmbus_ring_hdr *rx_ring;
    uint8_t               *rx_data;
    uint32_t               rx_size;

    void (*callback)(struct vmbus_channel *ch, void *ctx);
    void *ctx;

    int sint;
    int in_use;
};

struct vmbus_msg_hdr {
    uint32_t type;
    uint32_t padding;
} __attribute__((packed));

struct vmbus_msg_initiate_contact {
    struct vmbus_msg_hdr hdr;
    uint32_t version;
    uint32_t target_vcpu;
    uint64_t interrupt_page;
    uint64_t monitor_page1;
    uint64_t monitor_page2;
} __attribute__((packed));

struct vmbus_msg_version_response {
    struct vmbus_msg_hdr hdr;
    uint8_t  version_supported;
    uint8_t  padding[3];
} __attribute__((packed));

struct vmbus_msg_offer {
    struct vmbus_msg_hdr hdr;
    vmbus_guid_t type_guid;
    vmbus_guid_t instance_guid;
    uint64_t reserved1;
    uint64_t reserved2;
    uint32_t child_relid;
    uint8_t  monitor_id;
    uint8_t  monitor_allocated;
    uint16_t is_dedicated;
    uint32_t connection_id;
} __attribute__((packed));

struct vmbus_msg_gpadl_header {
    struct vmbus_msg_hdr hdr;
    uint32_t child_relid;
    uint32_t gpadl;
    uint16_t range_buflen;
    uint16_t rangecount;
    uint32_t range_len;
    uint32_t range_offset;
    uint64_t pfn[];
} __attribute__((packed));

struct vmbus_msg_gpadl_created {
    struct vmbus_msg_hdr hdr;
    uint32_t child_relid;
    uint32_t gpadl;
    uint32_t status;
} __attribute__((packed));

struct vmbus_msg_openchannel {
    struct vmbus_msg_hdr hdr;
    uint32_t child_relid;
    uint32_t open_id;
    uint32_t ring_buffer_gpadl;
    uint32_t target_vcpu;
    uint32_t downstream_offset;
    uint8_t  userdata[120];
} __attribute__((packed));

struct vmbus_msg_openchannel_result {
    struct vmbus_msg_hdr hdr;
    uint32_t child_relid;
    uint32_t open_id;
    uint32_t status;
} __attribute__((packed));

#define VMBUS_PKT_DATA_INBAND    6
#define VMBUS_PKT_DATA_GPA       7
#define VMBUS_PKT_COMP           11

int vmbus_init(void);

struct vmbus_channel *vmbus_find_channel(const uint8_t guid[16]);

int vmbus_open(struct vmbus_channel *ch, uint32_t ring_size,
               void (*callback)(struct vmbus_channel *, void *), void *ctx);

int vmbus_send(struct vmbus_channel *ch, const void *data, size_t len);

int vmbus_recv(struct vmbus_channel *ch, void *buf, size_t bufsize);

int vmbus_send_pkt(struct vmbus_channel *ch, uint64_t type,
                   const void *hdr, size_t hdr_len,
                   const void *data, size_t data_len);

int vmbus_recv_pkt(struct vmbus_channel *ch, uint64_t *type,
                   void *buf, size_t bufsize);

void vmbus_close(struct vmbus_channel *ch);

void vmbus_signal(struct vmbus_channel *ch);

#endif
