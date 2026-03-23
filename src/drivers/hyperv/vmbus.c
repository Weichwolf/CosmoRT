/* CosmoRT VMBus Transport — channel management, ring buffers, signaling
 *
 * Talks to Hyper-V host via HvPostMessage hypercall.
 * Ring buffers are shared memory (GPADL) for high-throughput data transfer.
 *
 * Only imports: hw.h, config.h, serial.h, hyperv.h, vmbus.h
 */

#include "vmbus.h"
#include "cosmo.h"
#include "hyperv.h"

/* ---- State ---- */

static struct vmbus_channel channels[VMBUS_MAX_CHANNELS];
static int channel_count;
static hw_spinlock_t vmbus_lock = HW_SPINLOCK_INIT;
static volatile int version_ok;
static volatile int offers_done;
static volatile int gpadl_created;
static volatile uint32_t gpadl_status;
static volatile int open_result;
static volatile uint32_t open_status;
static uint32_t next_gpadl = 1;
static uint32_t next_open_id = 1;

/* ---- VMBus packet header (on-wire in ring buffer) ---- */

struct vmbus_pkt_hdr {
    uint16_t type;
    uint16_t offset8;      /* offset to data in 8-byte units from start of pkt */
    uint16_t len8;         /* total packet length in 8-byte units */
    uint16_t flags;
    uint64_t trans_id;
} __attribute__((packed));

/* ---- Ring buffer operations ---- */

static uint32_t ring_read_avail(struct vmbus_ring_hdr *ring, uint32_t data_size) {
    uint32_t wi = ring->write_index;
    uint32_t ri = ring->read_index;
    if (wi >= ri)
        return wi - ri;
    return data_size - ri + wi;
}

static uint32_t ring_write_avail(struct vmbus_ring_hdr *ring, uint32_t data_size) {
    uint32_t avail = ring_read_avail(ring, data_size);
    /* One slot reserved to distinguish full from empty */
    return data_size - avail - 1;
}

static void ring_read(uint8_t *ring_data, uint32_t data_size,
                      uint32_t *read_idx, void *buf, size_t len) {
    uint32_t ri = *read_idx;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        dst[i] = ring_data[ri];
        ri = (ri + 1) % data_size;
    }
    *read_idx = ri;
}

static void ring_write(uint8_t *ring_data, uint32_t data_size,
                       uint32_t *write_idx, const void *buf, size_t len) {
    uint32_t wi = *write_idx;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        ring_data[wi] = src[i];
        wi = (wi + 1) % data_size;
    }
    *write_idx = wi;
}

/* ---- Message handler (called from SynIC ISR via hyperv.c) ---- */

static void vmbus_on_message(void) {
    struct hv_message *slot = hyperv_simp_slot(HV_VMBUS_MSG_SINT);
    if (!slot || slot->type == 0)
        return;

    /* The payload starts with vmbus_msg_hdr */
    struct vmbus_msg_hdr *hdr = (struct vmbus_msg_hdr *)slot->payload;

    switch (hdr->type) {
    case CHANNELMSG_VERSION_RESPONSE: {
        struct vmbus_msg_version_response *resp =
            (struct vmbus_msg_version_response *)slot->payload;
        version_ok = resp->version_supported ? 1 : -1;
        break;
    }

    case CHANNELMSG_OFFERCHANNEL: {
        struct vmbus_msg_offer *offer =
            (struct vmbus_msg_offer *)slot->payload;
        if (channel_count < VMBUS_MAX_CHANNELS) {
            struct vmbus_channel *ch = &channels[channel_count];
            hw_memset(ch, 0, sizeof(*ch));
            ch->child_relid = offer->child_relid;
            hw_memcpy(&ch->type_guid, &offer->type_guid, 16);
            hw_memcpy(&ch->instance_guid, &offer->instance_guid, 16);
            ch->monitor_id = offer->monitor_id;
            ch->sint = HV_VMBUS_MSG_SINT;
            ch->in_use = 1;
            channel_count++;
        }
        break;
    }

    case CHANNELMSG_ALLOFFERS_DELIVERED:
        offers_done = 1;
        break;

    case CHANNELMSG_GPADL_CREATED: {
        struct vmbus_msg_gpadl_created *g =
            (struct vmbus_msg_gpadl_created *)slot->payload;
        gpadl_status = g->status;
        gpadl_created = 1;
        break;
    }

    case CHANNELMSG_OPENCHANNEL_RESULT: {
        struct vmbus_msg_openchannel_result *r =
            (struct vmbus_msg_openchannel_result *)slot->payload;
        open_status = r->status;
        open_result = 1;
        break;
    }

    default:
        serial_puts("vmbus: unknown msg type ");
        serial_hex64(hdr->type);
        serial_putchar('\n');
        break;
    }

    /* Acknowledge message slot */
    hyperv_msg_recv(HV_VMBUS_MSG_SINT, 0, 0);
}

/* Event handler: dispatch to channel callbacks */
static void vmbus_on_event(void) {
    for (int i = 0; i < channel_count; i++) {
        struct vmbus_channel *ch = &channels[i];
        if (ch->state == 1 && ch->rx_ring && ch->callback) {
            if (ring_read_avail(ch->rx_ring, ch->rx_size) > 0)
                ch->callback(ch, ch->ctx);
        }
    }
}

/* ---- Wait helpers (with timeout) ---- */

static int wait_flag(volatile int *flag, uint64_t timeout_ms) {
    uint64_t deadline = hw_ms() + timeout_ms;
    while (!*flag) {
        if (hw_ms() > deadline) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

/* ---- Init ---- */

int vmbus_init(void) {
    channel_count = 0;
    version_ok = 0;
    offers_done = 0;

    /* Register SynIC message/event handlers */
    extern void hyperv_set_vmbus_msg_handler(void (*fn)(void));
    extern void hyperv_set_vmbus_evt_handler(void (*fn)(void));
    hyperv_set_vmbus_msg_handler(vmbus_on_message);
    hyperv_set_vmbus_evt_handler(vmbus_on_event);

    /* Try protocol versions in order */
    static const uint32_t versions[] = {
        VMBUS_VERSION_WIN10,
        VMBUS_VERSION_WIN8_1,
        VMBUS_VERSION_WIN8,
    };

    int connected = 0;
    for (int v = 0; v < 3; v++) {
        struct vmbus_msg_initiate_contact msg;
        hw_memset(&msg, 0, sizeof(msg));
        msg.hdr.type = CHANNELMSG_INITIATE_CONTACT;
        msg.version = versions[v];
        msg.target_vcpu = 0;
        msg.interrupt_page = 0;
        msg.monitor_page1 = 0;
        msg.monitor_page2 = 0;

        version_ok = 0;
        uint64_t r = hyperv_post_message(VMBUS_MESSAGE_CONNECTION_ID,
                                          1 /* VMBUS type */,
                                          &msg, sizeof(msg));
        if (r != 0) continue;

        if (wait_flag(&version_ok, 2000) < 0) continue;
        if (version_ok == 1) {
            serial_puts("vmbus: connected version ");
            serial_hex64(versions[v]);
            serial_putchar('\n');
            connected = 1;
            break;
        }
    }

    if (!connected) {
        serial_puts("vmbus: version negotiation failed\n");
        return -1;
    }

    /* Request channel offers */
    struct vmbus_msg_hdr req;
    hw_memset(&req, 0, sizeof(req));
    req.type = CHANNELMSG_REQUESTOFFERS;

    hyperv_post_message(VMBUS_MESSAGE_CONNECTION_ID, 1, &req, sizeof(req));

    if (wait_flag(&offers_done, 5000) < 0) {
        serial_puts("vmbus: offer timeout\n");
        return -1;
    }

    serial_puts("vmbus: ");
    serial_putchar('0' + (channel_count / 10));
    serial_putchar('0' + (channel_count % 10));
    serial_puts(" channels offered\n");

    return 0;
}

/* ---- Channel lookup ---- */

static int guid_match(const vmbus_guid_t *a, const uint8_t b[16]) {
    for (int i = 0; i < 16; i++)
        if (a->data[i] != b[i]) return 0;
    return 1;
}

struct vmbus_channel *vmbus_find_channel(const uint8_t guid[16]) {
    for (int i = 0; i < channel_count; i++) {
        if (guid_match(&channels[i].type_guid, guid))
            return &channels[i];
    }
    return 0;
}

/* ---- GPADL creation ---- */

static int create_gpadl(struct vmbus_channel *ch, uint64_t phys, uint32_t size,
                         uint32_t *handle_out) {
    uint32_t npages = (size + 4095) / 4096;
    uint32_t handle = __sync_fetch_and_add(&next_gpadl, 1);

    /* Build GPADL header message.
     * Max payload in HvPostMessage = 240 bytes.
     * Header overhead ~32 bytes, each PFN = 8 bytes → ~25 PFNs per message.
     * For ring_size <= 128KB (32 pages) this fits in one message. */
    size_t msg_size = sizeof(struct vmbus_msg_gpadl_header) + npages * 8;
    if (msg_size > 240) {
        serial_puts("vmbus: GPADL too large for single message\n");
        return -1;
    }

    uint8_t buf[240];
    hw_memset(buf, 0, sizeof(buf));
    struct vmbus_msg_gpadl_header *ghdr = (struct vmbus_msg_gpadl_header *)buf;
    ghdr->hdr.type = CHANNELMSG_GPADL_HEADER;
    ghdr->child_relid = ch->child_relid;
    ghdr->gpadl = handle;
    ghdr->range_buflen = (uint16_t)(8 + npages * 8);
    ghdr->rangecount = 1;
    ghdr->range_len = size;
    ghdr->range_offset = 0;

    uint64_t base_pfn = phys >> 12;
    for (uint32_t i = 0; i < npages; i++)
        ghdr->pfn[i] = base_pfn + i;

    gpadl_created = 0;
    uint64_t r = hyperv_post_message(VMBUS_MESSAGE_CONNECTION_ID, 1, buf, msg_size);
    if (r != 0) return -1;

    if (wait_flag(&gpadl_created, 5000) < 0) {
        serial_puts("vmbus: GPADL creation timeout\n");
        return -1;
    }

    if (gpadl_status != 0) {
        serial_puts("vmbus: GPADL creation failed status=");
        serial_hex64(gpadl_status);
        serial_putchar('\n');
        return -1;
    }

    *handle_out = handle;
    return 0;
}

/* ---- Open channel ---- */

int vmbus_open(struct vmbus_channel *ch, uint32_t ring_size,
               void (*callback)(struct vmbus_channel *, void *), void *ctx) {
    if (!ch || ch->state != 0) return -1;

    /* Allocate ring buffer memory (tx ring + rx ring) */
    uint32_t total = ring_size * 2;
    if (cosmo_dma_alloc(total, &ch->ring_mem, &ch->ring_phys) < 0) {
        serial_puts("vmbus: ring alloc failed\n");
        return -1;
    }
    hw_memset(ch->ring_mem, 0, total);
    ch->ring_size = total;

    /* Setup ring pointers */
    ch->tx_ring = (struct vmbus_ring_hdr *)ch->ring_mem;
    ch->tx_data = (uint8_t *)ch->ring_mem + VMBUS_RING_HDR_SIZE;
    ch->tx_size = ring_size - VMBUS_RING_HDR_SIZE;

    ch->rx_ring = (struct vmbus_ring_hdr *)((uint8_t *)ch->ring_mem + ring_size);
    ch->rx_data = (uint8_t *)ch->ring_mem + ring_size + VMBUS_RING_HDR_SIZE;
    ch->rx_size = ring_size - VMBUS_RING_HDR_SIZE;

    ch->callback = callback;
    ch->ctx = ctx;

    /* Create GPADL for the ring buffer */
    if (create_gpadl(ch, ch->ring_phys, total, &ch->gpadl_handle) < 0) {
        cosmo_dma_free(ch->ring_mem, total);
        ch->ring_mem = 0;
        return -1;
    }

    /* Send OPENCHANNEL */
    struct vmbus_msg_openchannel open_msg;
    hw_memset(&open_msg, 0, sizeof(open_msg));
    open_msg.hdr.type = CHANNELMSG_OPENCHANNEL;
    open_msg.child_relid = ch->child_relid;
    open_msg.open_id = __sync_fetch_and_add(&next_open_id, 1);
    open_msg.ring_buffer_gpadl = ch->gpadl_handle;
    open_msg.target_vcpu = 0;
    open_msg.downstream_offset = ring_size;  /* rx ring starts at this offset */

    open_result = 0;
    uint64_t r = hyperv_post_message(VMBUS_MESSAGE_CONNECTION_ID, 1,
                                      &open_msg, sizeof(open_msg));
    if (r != 0) return -1;

    if (wait_flag(&open_result, 5000) < 0) {
        serial_puts("vmbus: open timeout\n");
        return -1;
    }

    if (open_status != 0) {
        serial_puts("vmbus: open failed status=");
        serial_hex64(open_status);
        serial_putchar('\n');
        return -1;
    }

    ch->state = 1;  /* open */
    return 0;
}

/* ---- Send / Receive ---- */

void vmbus_signal(struct vmbus_channel *ch) {
    /* Signal the host via event connection (child_relid as connection) */
    hyperv_signal_event(ch->child_relid);
}

int vmbus_send(struct vmbus_channel *ch, const void *data, size_t len) {
    if (!ch || ch->state != 1 || !ch->tx_ring) return -1;

    uint64_t flags;
    hw_spin_lock_irq((hw_spinlock_t *)&vmbus_lock, &flags);

    uint32_t avail = ring_write_avail(ch->tx_ring, ch->tx_size);
    uint32_t needed = (uint32_t)(len + sizeof(uint64_t));  /* data + padding/size trailer */
    needed = (needed + 7) & ~7u;  /* 8-byte align */

    if (avail < needed) {
        hw_spin_unlock_irq((hw_spinlock_t *)&vmbus_lock, flags);
        return -1;
    }

    uint32_t wi = ch->tx_ring->write_index;
    ring_write(ch->tx_data, ch->tx_size, &wi, data, len);

    /* Pad to 8-byte alignment */
    uint32_t pad = ((len + 7) & ~7u) - len;
    if (pad) {
        uint8_t zeros[8] = {0};
        ring_write(ch->tx_data, ch->tx_size, &wi, zeros, pad);
    }

    /* Write previous write_index as trailer (for host) */
    uint64_t prev_wi = ch->tx_ring->write_index;
    ring_write(ch->tx_data, ch->tx_size, &wi, &prev_wi, sizeof(prev_wi));

    __asm__ volatile("mfence" ::: "memory");
    ch->tx_ring->write_index = wi;
    __asm__ volatile("mfence" ::: "memory");

    hw_spin_unlock_irq((hw_spinlock_t *)&vmbus_lock, flags);

    vmbus_signal(ch);
    return 0;
}

int vmbus_recv(struct vmbus_channel *ch, void *buf, size_t bufsize) {
    if (!ch || ch->state != 1 || !ch->rx_ring) return 0;

    uint32_t avail = ring_read_avail(ch->rx_ring, ch->rx_size);
    if (avail == 0) return 0;

    /* Read available data (up to bufsize) */
    uint32_t to_read = avail;
    if (to_read > (uint32_t)bufsize) to_read = (uint32_t)bufsize;

    uint32_t ri = ch->rx_ring->read_index;
    ring_read(ch->rx_data, ch->rx_size, &ri, buf, to_read);

    __asm__ volatile("mfence" ::: "memory");
    ch->rx_ring->read_index = ri;
    __asm__ volatile("mfence" ::: "memory");

    return (int)to_read;
}

int vmbus_send_pkt(struct vmbus_channel *ch, uint64_t type,
                   const void *hdr, size_t hdr_len,
                   const void *data, size_t data_len) {
    if (!ch || ch->state != 1) return -1;

    /* Build packet: vmbus_pkt_hdr + header data + payload */
    struct vmbus_pkt_hdr pkt;
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.type = (uint16_t)type;
    uint32_t total = (uint32_t)(sizeof(pkt) + hdr_len + data_len);
    total = (total + 7) & ~7u;
    pkt.len8 = (uint16_t)(total / 8);
    pkt.offset8 = (uint16_t)((sizeof(pkt) + hdr_len + 7) / 8);
    static uint64_t trans_id_counter;
    pkt.trans_id = __sync_fetch_and_add(&trans_id_counter, 1);

    uint64_t flags;
    hw_spin_lock_irq((hw_spinlock_t *)&vmbus_lock, &flags);

    uint32_t avail = ring_write_avail(ch->tx_ring, ch->tx_size);
    uint32_t needed = total + 8;  /* packet + trailer */
    if (avail < needed) {
        hw_spin_unlock_irq((hw_spinlock_t *)&vmbus_lock, flags);
        return -1;
    }

    uint32_t wi = ch->tx_ring->write_index;
    ring_write(ch->tx_data, ch->tx_size, &wi, &pkt, sizeof(pkt));
    if (hdr_len)
        ring_write(ch->tx_data, ch->tx_size, &wi, hdr, hdr_len);
    if (data_len)
        ring_write(ch->tx_data, ch->tx_size, &wi, data, data_len);

    /* Pad */
    uint32_t written = (uint32_t)(sizeof(pkt) + hdr_len + data_len);
    uint32_t pad = total - written;
    if (pad) {
        uint8_t zeros[8] = {0};
        ring_write(ch->tx_data, ch->tx_size, &wi, zeros, pad);
    }

    /* Trailer: previous write index */
    uint64_t prev_wi = ch->tx_ring->write_index;
    ring_write(ch->tx_data, ch->tx_size, &wi, &prev_wi, 8);

    __asm__ volatile("mfence" ::: "memory");
    ch->tx_ring->write_index = wi;
    __asm__ volatile("mfence" ::: "memory");

    hw_spin_unlock_irq((hw_spinlock_t *)&vmbus_lock, flags);

    vmbus_signal(ch);
    return 0;
}

int vmbus_recv_pkt(struct vmbus_channel *ch, uint64_t *type,
                   void *buf, size_t bufsize) {
    if (!ch || ch->state != 1 || !ch->rx_ring) return 0;

    uint32_t avail = ring_read_avail(ch->rx_ring, ch->rx_size);
    if (avail < sizeof(struct vmbus_pkt_hdr))
        return 0;

    /* Peek at header without advancing read index */
    uint32_t ri = ch->rx_ring->read_index;
    struct vmbus_pkt_hdr pkt;
    ring_read(ch->rx_data, ch->rx_size, &ri, &pkt, sizeof(pkt));

    uint32_t total_bytes = (uint32_t)pkt.len8 * 8;
    if (avail < total_bytes + 8)  /* packet + trailer */
        return 0;

    if (type) *type = pkt.type;

    /* Read payload (skip pkt header, already consumed) */
    uint32_t payload_off = (uint32_t)pkt.offset8 * 8;
    uint32_t hdr_skip = payload_off - (uint32_t)sizeof(pkt);
    uint32_t data_len = total_bytes - payload_off;

    /* Skip intermediate header bytes */
    if (hdr_skip > 0) {
        uint8_t skip[128];
        while (hdr_skip > 0) {
            uint32_t chunk = hdr_skip > 128 ? 128 : hdr_skip;
            ring_read(ch->rx_data, ch->rx_size, &ri, skip, chunk);
            hdr_skip -= chunk;
        }
    }

    /* Read data payload */
    uint32_t to_read = data_len;
    if (to_read > (uint32_t)bufsize) to_read = (uint32_t)bufsize;
    if (to_read > 0)
        ring_read(ch->rx_data, ch->rx_size, &ri, buf, to_read);

    /* Skip remaining bytes + padding + trailer */
    uint32_t remaining = total_bytes - payload_off - to_read;
    remaining += 8;  /* trailer */
    while (remaining > 0) {
        uint8_t skip[128];
        uint32_t chunk = remaining > 128 ? 128 : remaining;
        ring_read(ch->rx_data, ch->rx_size, &ri, skip, chunk);
        remaining -= chunk;
    }

    __asm__ volatile("mfence" ::: "memory");
    ch->rx_ring->read_index = ri;
    __asm__ volatile("mfence" ::: "memory");

    return (int)to_read;
}

void vmbus_close(struct vmbus_channel *ch) {
    if (!ch || ch->state != 1) return;

    struct vmbus_msg_hdr close_msg;
    hw_memset(&close_msg, 0, sizeof(close_msg));
    close_msg.type = CHANNELMSG_CLOSECHANNEL;

    /* Reuse the relid field layout: type(4) + padding(4) + child_relid follows.
     * Actually, close channel message is: hdr + child_relid. */
    uint8_t buf[16];
    hw_memset(buf, 0, sizeof(buf));
    struct vmbus_msg_hdr *hdr = (struct vmbus_msg_hdr *)buf;
    hdr->type = CHANNELMSG_CLOSECHANNEL;
    *(uint32_t *)(buf + 8) = ch->child_relid;

    hyperv_post_message(VMBUS_MESSAGE_CONNECTION_ID, 1, buf, 12);

    ch->state = 2;
    ch->callback = 0;

    if (ch->ring_mem) {
        cosmo_dma_free(ch->ring_mem, ch->ring_size);
        ch->ring_mem = 0;
    }
}
