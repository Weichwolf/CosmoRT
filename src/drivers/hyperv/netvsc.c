/* CosmoRT netvsc — Hyper-V Synthetic Network Adapter */

#include "vmbus.h"
#include "cosmort.h"

static const uint8_t NETVSC_GUID[16] = {
    0x63, 0x51, 0x61, 0xf8, 0x3e, 0xdf, 0xc5, 0x46,
    0x91, 0x3f, 0xf2, 0xd2, 0xf9, 0x65, 0xed, 0x0e
};

#define NVSP_MSG_TYPE_INIT            1
#define NVSP_MSG_TYPE_INIT_COMPLETE   2
#define NVSP_MSG_TYPE_SEND_NDIS_VER   100
#define NVSP_MSG_TYPE_SEND_RCV_BUF   101
#define NVSP_MSG_TYPE_SEND_RCV_BUF_COMPLETE 102
#define NVSP_MSG_TYPE_REVOKE_RCV_BUF 103
#define NVSP_MSG_TYPE_SEND_SEND_BUF  104
#define NVSP_MSG_TYPE_SEND_SEND_BUF_COMPLETE 105
#define NVSP_MSG_TYPE_SEND_RNDIS_PKT 107
#define NVSP_MSG_TYPE_SEND_RNDIS_PKT_COMPLETE 108

#define NVSP_PROTOCOL_VERSION_5      0x30002

#define RNDIS_MSG_PACKET             0x00000001
#define RNDIS_MSG_INIT               0x00000002
#define RNDIS_MSG_INIT_COMPLETE      0x80000002
#define RNDIS_MSG_QUERY              0x00000004
#define RNDIS_MSG_QUERY_COMPLETE     0x80000004
#define RNDIS_MSG_SET                0x00000005
#define RNDIS_MSG_SET_COMPLETE       0x80000005

#define OID_802_3_PERMANENT_ADDRESS  0x01010101
#define OID_802_3_CURRENT_ADDRESS    0x01010102
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010E

#define NDIS_PACKET_TYPE_DIRECTED    0x00000001
#define NDIS_PACKET_TYPE_MULTICAST   0x00000002
#define NDIS_PACKET_TYPE_ALL_MULTICAST 0x00000004
#define NDIS_PACKET_TYPE_BROADCAST   0x00000008

struct nvsp_msg_hdr {
    uint32_t msg_type;
} __attribute__((packed));

struct nvsp_msg_init {
    struct nvsp_msg_hdr hdr;
    uint32_t min_protocol_ver;
    uint32_t max_protocol_ver;
    uint32_t padding[4];
} __attribute__((packed));

struct nvsp_msg_init_complete {
    struct nvsp_msg_hdr hdr;
    uint32_t negotiated_prot_ver;
    uint32_t max_mdl_chain_len;
    uint32_t status;
    uint32_t padding[3];
} __attribute__((packed));

struct nvsp_msg_send_rndis {
    struct nvsp_msg_hdr hdr;
    uint32_t channel_type;
    uint32_t send_buf_section_index;
    uint32_t send_buf_section_size;
    uint32_t padding[3];
} __attribute__((packed));

struct rndis_msg_hdr {
    uint32_t msg_type;
    uint32_t msg_len;
} __attribute__((packed));

struct rndis_init {
    struct rndis_msg_hdr hdr;
    uint32_t request_id;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t max_xfer_size;
} __attribute__((packed));

struct rndis_init_complete {
    struct rndis_msg_hdr hdr;
    uint32_t request_id;
    uint32_t status;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t device_flags;
    uint32_t medium;
    uint32_t max_pkt_per_msg;
    uint32_t max_xfer_size;
    uint32_t pkt_alignment;
    uint32_t af_list_offset;
    uint32_t af_list_size;
} __attribute__((packed));

struct rndis_query {
    struct rndis_msg_hdr hdr;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buf_len;
    uint32_t info_buf_offset;
    uint32_t reserved;
} __attribute__((packed));

struct rndis_query_complete {
    struct rndis_msg_hdr hdr;
    uint32_t request_id;
    uint32_t status;
    uint32_t info_buf_len;
    uint32_t info_buf_offset;
} __attribute__((packed));

struct rndis_set {
    struct rndis_msg_hdr hdr;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buf_len;
    uint32_t info_buf_offset;
    uint32_t reserved;
} __attribute__((packed));

struct rndis_packet {
    struct rndis_msg_hdr hdr;
    uint32_t data_offset;
    uint32_t data_len;
    uint32_t oob_data_offset;
    uint32_t oob_data_len;
    uint32_t num_oob_elements;
    uint32_t per_pkt_info_offset;
    uint32_t per_pkt_info_len;
    uint32_t reserved1;
    uint32_t reserved2;
} __attribute__((packed));

static struct vmbus_channel *net_ch;
static uint8_t netvsc_mac[6];
static volatile int nvsp_init_done;
static volatile int rndis_resp_ready;
static uint8_t rndis_resp_buf[512];
static int rndis_resp_len;
static uint32_t rndis_req_id = 1;
static hw_spinlock_t net_lock = HW_SPINLOCK_INIT;

#define NETVSC_RX_QUEUE 16
#define NETVSC_PKT_SIZE 1536
static uint8_t rx_pkts[NETVSC_RX_QUEUE][NETVSC_PKT_SIZE];
static int     rx_lens[NETVSC_RX_QUEUE];
static int     rx_head, rx_count;

static int send_rndis(const void *rndis_msg, size_t rndis_len) {
    struct nvsp_msg_send_rndis nvsp;
    hw_memset(&nvsp, 0, sizeof(nvsp));
    nvsp.hdr.msg_type = NVSP_MSG_TYPE_SEND_RNDIS_PKT;
    nvsp.channel_type = 0;
    nvsp.send_buf_section_index = 0xFFFFFFFF;
    nvsp.send_buf_section_size = 0;

    return vmbus_send_pkt(net_ch, VMBUS_PKT_DATA_INBAND,
                           &nvsp, sizeof(nvsp),
                           rndis_msg, rndis_len);
}

static void netvsc_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[2048];
    int len;

    while ((len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf))) > 0) {
        if (pkt_type == VMBUS_PKT_DATA_INBAND) {
            struct nvsp_msg_hdr *hdr = (struct nvsp_msg_hdr *)buf;

            if (hdr->msg_type == NVSP_MSG_TYPE_INIT_COMPLETE) {
                nvsp_init_done = 1;
            } else if (hdr->msg_type == NVSP_MSG_TYPE_SEND_RCV_BUF_COMPLETE) {
                nvsp_init_done = 2;
            } else if (hdr->msg_type == NVSP_MSG_TYPE_SEND_RNDIS_PKT ||
                       hdr->msg_type == NVSP_MSG_TYPE_SEND_RNDIS_PKT_COMPLETE) {
                size_t nvsp_hdr_size = sizeof(struct nvsp_msg_send_rndis);
                if ((size_t)len > nvsp_hdr_size) {
                    uint8_t *rndis_data = buf + nvsp_hdr_size;
                    int rndis_len_actual = len - (int)nvsp_hdr_size;
                    struct rndis_msg_hdr *rmsg = (struct rndis_msg_hdr *)rndis_data;

                    if (rmsg->msg_type == RNDIS_MSG_INIT_COMPLETE ||
                        rmsg->msg_type == RNDIS_MSG_QUERY_COMPLETE ||
                        rmsg->msg_type == RNDIS_MSG_SET_COMPLETE) {
                        if (rndis_len_actual > (int)sizeof(rndis_resp_buf))
                            rndis_len_actual = (int)sizeof(rndis_resp_buf);
                        hw_memcpy(rndis_resp_buf, rndis_data, (size_t)rndis_len_actual);
                        rndis_resp_len = rndis_len_actual;
                        rndis_resp_ready = 1;
                    } else if (rmsg->msg_type == RNDIS_MSG_PACKET) {
                        struct rndis_packet *rpkt = (struct rndis_packet *)rndis_data;
                        uint32_t data_off = rpkt->data_offset + 8;
                        uint32_t data_len_pkt = rpkt->data_len;
                        if (data_off + data_len_pkt <= (uint32_t)rndis_len_actual &&
                            data_len_pkt <= NETVSC_PKT_SIZE && rx_count < NETVSC_RX_QUEUE) {
                            int idx = (rx_head + rx_count) % NETVSC_RX_QUEUE;
                            hw_memcpy(rx_pkts[idx], rndis_data + data_off, data_len_pkt);
                            rx_lens[idx] = (int)data_len_pkt;
                            rx_count++;
                        }
                    }
                }
            }
        } else if (pkt_type == VMBUS_PKT_COMP) {
            if ((size_t)len >= sizeof(struct nvsp_msg_hdr)) {
                struct nvsp_msg_hdr *hdr = (struct nvsp_msg_hdr *)buf;
                if (hdr->msg_type == NVSP_MSG_TYPE_SEND_RNDIS_PKT_COMPLETE) {
                }
            }
        }
    }
}

static int wait_rndis(volatile int *flag, uint64_t timeout_ms) {
    uint64_t deadline = hw_ms() + timeout_ms;
    while (!*flag) {
        if (hw_ms() > deadline) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

static int rndis_init_msg(void) {
    struct rndis_init msg;
    hw_memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = RNDIS_MSG_INIT;
    msg.hdr.msg_len = sizeof(msg);
    msg.request_id = rndis_req_id++;
    msg.major_version = 1;
    msg.minor_version = 0;
    msg.max_xfer_size = 2048;

    rndis_resp_ready = 0;
    if (send_rndis(&msg, sizeof(msg)) < 0) return -1;
    if (wait_rndis(&rndis_resp_ready, 5000) < 0) return -1;

    struct rndis_init_complete *resp = (struct rndis_init_complete *)rndis_resp_buf;
    if (resp->status != 0) {
        serial_puts("netvsc: RNDIS init failed status=");
        serial_hex64(resp->status);
        serial_putchar('\n');
        return -1;
    }
    return 0;
}

static int rndis_query_mac(void) {
    uint8_t msg_buf[64];
    hw_memset(msg_buf, 0, sizeof(msg_buf));
    struct rndis_query *q = (struct rndis_query *)msg_buf;
    q->hdr.msg_type = RNDIS_MSG_QUERY;
    q->hdr.msg_len = sizeof(struct rndis_query);
    q->request_id = rndis_req_id++;
    q->oid = OID_802_3_PERMANENT_ADDRESS;
    q->info_buf_len = 0;
    q->info_buf_offset = 0;

    rndis_resp_ready = 0;
    if (send_rndis(q, q->hdr.msg_len) < 0) return -1;
    if (wait_rndis(&rndis_resp_ready, 5000) < 0) return -1;

    struct rndis_query_complete *resp = (struct rndis_query_complete *)rndis_resp_buf;
    if (resp->status != 0) return -1;

    uint8_t *mac_data = (uint8_t *)&resp->request_id + resp->info_buf_offset;
    hw_memcpy(netvsc_mac, mac_data, 6);
    return 0;
}

static int rndis_set_packet_filter(void) {
    uint8_t msg_buf[64];
    hw_memset(msg_buf, 0, sizeof(msg_buf));
    struct rndis_set *s = (struct rndis_set *)msg_buf;
    s->hdr.msg_type = RNDIS_MSG_SET;
    s->hdr.msg_len = sizeof(struct rndis_set) + 4;
    s->request_id = rndis_req_id++;
    s->oid = OID_GEN_CURRENT_PACKET_FILTER;
    s->info_buf_len = 4;
    s->info_buf_offset = 20;

    uint32_t filter = NDIS_PACKET_TYPE_DIRECTED |
                      NDIS_PACKET_TYPE_BROADCAST |
                      NDIS_PACKET_TYPE_ALL_MULTICAST;
    hw_memcpy(msg_buf + sizeof(struct rndis_set), &filter, 4);

    rndis_resp_ready = 0;
    if (send_rndis(msg_buf, s->hdr.msg_len) < 0) return -1;
    if (wait_rndis(&rndis_resp_ready, 5000) < 0) return -1;

    return 0;
}

static int netvsc_send(const void *data, uint16_t len) {
    if (!net_ch || net_ch->state != 1) return -1;

    struct rndis_packet rpkt;
    hw_memset(&rpkt, 0, sizeof(rpkt));
    rpkt.hdr.msg_type = RNDIS_MSG_PACKET;
    rpkt.hdr.msg_len = (uint32_t)(sizeof(rpkt) + len);
    rpkt.data_offset = sizeof(rpkt) - 8;
    rpkt.data_len = len;

    struct nvsp_msg_send_rndis nvsp;
    hw_memset(&nvsp, 0, sizeof(nvsp));
    nvsp.hdr.msg_type = NVSP_MSG_TYPE_SEND_RNDIS_PKT;
    nvsp.channel_type = 0;
    nvsp.send_buf_section_index = 0xFFFFFFFF;

    uint8_t payload[2048];
    if (sizeof(rpkt) + len > sizeof(payload)) return -1;
    hw_memcpy(payload, &rpkt, sizeof(rpkt));
    hw_memcpy(payload + sizeof(rpkt), data, len);

    return vmbus_send_pkt(net_ch, VMBUS_PKT_DATA_INBAND,
                           &nvsp, sizeof(nvsp),
                           payload, sizeof(rpkt) + len);
}

static int netvsc_recv(void *buf, uint16_t bufsize) {
    if (rx_count == 0) return 0;

    uint64_t flags;
    hw_spin_lock_irq(&net_lock, &flags);

    if (rx_count == 0) {
        hw_spin_unlock_irq(&net_lock, flags);
        return 0;
    }

    int len = rx_lens[rx_head];
    if (len > (int)bufsize) len = (int)bufsize;
    hw_memcpy(buf, rx_pkts[rx_head], (size_t)len);

    rx_head = (rx_head + 1) % NETVSC_RX_QUEUE;
    rx_count--;

    hw_spin_unlock_irq(&net_lock, flags);
    return len;
}

static void netvsc_get_mac(uint8_t mac[6]) {
    hw_memcpy(mac, netvsc_mac, 6);
}

static const nic_driver_t netvsc_driver = {
    .send    = netvsc_send,
    .recv    = netvsc_recv,
    .get_mac = netvsc_get_mac,
    .name    = "netvsc"
};

int netvsc_init(void) {
    net_ch = vmbus_find_channel(NETVSC_GUID);
    if (!net_ch) {
        serial_puts("netvsc: no channel found\n");
        return -1;
    }
    serial_puts("netvsc: channel found, relid=");
    serial_hex64(net_ch->child_relid);
    serial_putchar('\n');

    if (vmbus_open(net_ch, 64 * 1024, netvsc_callback, 0) < 0) {
        serial_puts("netvsc: open failed\n");
        return -1;
    }

    struct nvsp_msg_init nvsp;
    hw_memset(&nvsp, 0, sizeof(nvsp));
    nvsp.hdr.msg_type = NVSP_MSG_TYPE_INIT;
    nvsp.min_protocol_ver = NVSP_PROTOCOL_VERSION_5;
    nvsp.max_protocol_ver = NVSP_PROTOCOL_VERSION_5;

    nvsp_init_done = 0;
    vmbus_send_pkt(net_ch, VMBUS_PKT_DATA_INBAND, &nvsp, sizeof(nvsp), 0, 0);
    if (wait_rndis(&nvsp_init_done, 5000) < 0) {
        serial_puts("netvsc: NVSP init timeout\n");
        return -1;
    }

    if (rndis_init_msg() < 0) {
        serial_puts("netvsc: RNDIS init failed\n");
        return -1;
    }

    if (rndis_query_mac() < 0) {
        serial_puts("netvsc: MAC query failed\n");
        return -1;
    }

    serial_puts("netvsc: MAC=");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        hex[0] = "0123456789abcdef"[netvsc_mac[i] >> 4];
        hex[1] = "0123456789abcdef"[netvsc_mac[i] & 0xF];
        hex[2] = 0;
        serial_puts(hex);
        if (i < 5) serial_putchar(':');
    }
    serial_putchar('\n');

    if (rndis_set_packet_filter() < 0) {
        serial_puts("netvsc: set filter failed\n");
        return -1;
    }

    net_nic_register(&netvsc_driver);
    serial_puts("netvsc: ready\n");
    return 0;
}
