/* CosmoRT VMBus Transport — channel management, ring buffers, signaling
 *
 * VMBus is the transport for ALL Hyper-V synthetic devices.
 * Ring buffer pairs (upstream + downstream) over shared memory.
 */
#ifndef VMBUS_H
#define VMBUS_H

#include <stdint.h>
#include <stddef.h>

/* Channel message types (host <-> guest) */
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

/* VMBus protocol versions */
#define VMBUS_VERSION_WIN10            ((5 << 16) | 0)
#define VMBUS_VERSION_WIN8_1           ((4 << 16) | 0)
#define VMBUS_VERSION_WIN8             ((3 << 16) | 0)

/* VMBus connection ID for HvPostMessage */
#define VMBUS_MESSAGE_CONNECTION_ID    1

/* Max channels */
#define VMBUS_MAX_CHANNELS             64

/* GUID type (16 bytes) */
typedef struct {
    uint8_t data[16];
} vmbus_guid_t;

/* Ring buffer header (at start of shared memory region) */
struct vmbus_ring_hdr {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t interrupt_mask;
    volatile uint32_t pending_send_size;
    uint32_t reserved[12];
    /* data follows: ring_size - sizeof(header) bytes */
};

#define VMBUS_RING_HDR_SIZE 64

/* VMBus channel */
struct vmbus_channel {
    uint32_t child_relid;
    vmbus_guid_t type_guid;
    vmbus_guid_t instance_guid;
    uint32_t monitor_id;
    int      state;          /* 0=offered, 1=open, 2=closed */

    /* Ring buffer (guest-side shared memory) */
    void    *ring_mem;       /* kernel virtual base */
    uint64_t ring_phys;      /* physical base */
    uint32_t ring_size;      /* total size (both rings) */
    uint32_t gpadl_handle;   /* GPADL handle assigned by host */

    /* Split ring pointers */
    struct vmbus_ring_hdr *tx_ring;   /* guest→host */
    uint8_t               *tx_data;
    uint32_t               tx_size;   /* data area size */
    struct vmbus_ring_hdr *rx_ring;   /* host→guest */
    uint8_t               *rx_data;
    uint32_t               rx_size;

    /* Callback on incoming data */
    void (*callback)(struct vmbus_channel *ch, void *ctx);
    void *ctx;

    int sint;               /* SINT used for this channel */
    int in_use;
};

/* ---- Channel message headers ---- */

struct vmbus_msg_hdr {
    uint32_t type;
    uint32_t padding;
} __attribute__((packed));

struct vmbus_msg_initiate_contact {
    struct vmbus_msg_hdr hdr;
    uint32_t version;
    uint32_t target_vcpu;
    uint64_t interrupt_page;   /* deprecated, set to 0 */
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
    /* Followed by GPA range descriptors */
    uint32_t range_len;      /* in bytes */
    uint32_t range_offset;
    uint64_t pfn[];          /* page frame numbers */
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
    uint32_t downstream_offset;  /* offset to rx ring within GPADL */
    uint8_t  userdata[120];
} __attribute__((packed));

struct vmbus_msg_openchannel_result {
    struct vmbus_msg_hdr hdr;
    uint32_t child_relid;
    uint32_t open_id;
    uint32_t status;
} __attribute__((packed));

/* VMBus packet types (on-wire in ring buffer) */
#define VMBUS_PKT_DATA_INBAND    6
#define VMBUS_PKT_DATA_GPA       7
#define VMBUS_PKT_COMP           11

/* ---- API ---- */

/* Initialize VMBus: negotiate version, request offers. */
int vmbus_init(void);

/* Find a channel by type GUID. Returns NULL if not found. */
struct vmbus_channel *vmbus_find_channel(const uint8_t guid[16]);

/* Open a channel: allocate ring buffer, create GPADL, send open request. */
int vmbus_open(struct vmbus_channel *ch, uint32_t ring_size,
               void (*callback)(struct vmbus_channel *, void *), void *ctx);

/* Send data on a channel (write to tx ring). */
int vmbus_send(struct vmbus_channel *ch, const void *data, size_t len);

/* Receive data from a channel (read from rx ring). Returns bytes read, 0 if empty. */
int vmbus_recv(struct vmbus_channel *ch, void *buf, size_t bufsize);

/* Send packet with type header (for storvsc/netvsc protocol wrappers) */
int vmbus_send_pkt(struct vmbus_channel *ch, uint64_t type,
                   const void *hdr, size_t hdr_len,
                   const void *data, size_t data_len);

/* Receive packet (strips VMBus packet header). */
int vmbus_recv_pkt(struct vmbus_channel *ch, uint64_t *type,
                   void *buf, size_t bufsize);

/* Close a channel. */
void vmbus_close(struct vmbus_channel *ch);

/* Signal the host that we wrote to a channel's tx ring. */
void vmbus_signal(struct vmbus_channel *ch);

#endif
