/* CosmoRT hv_utils — Hyper-V Integration Services (Heartbeat, Shutdown, Time Sync) */

#include "vmbus.h"
#include "cosmort.h"

static const uint8_t HV_HEARTBEAT_GUID[16] = {
    0x39, 0x4f, 0x16, 0x57, 0x15, 0x91, 0x78, 0x4e,
    0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d
};

static const uint8_t HV_SHUTDOWN_GUID[16] = {
    0x31, 0x60, 0x0b, 0x0e, 0x13, 0x52, 0x34, 0x49,
    0x81, 0x8b, 0x38, 0xd9, 0x0c, 0xed, 0x39, 0xdb
};

static const uint8_t HV_TIMESYNC_GUID[16] = {
    0x30, 0xe6, 0x27, 0x95, 0xae, 0xd0, 0x7b, 0x49,
    0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf
};

#define ICMSG_NEGOTIATE    0
#define ICMSG_HEARTBEAT    1
#define ICMSG_SHUTDOWN     3
#define ICMSG_TIMESYNC     4

#define IC_VERSION_MAJOR   3
#define IC_VERSION_MINOR   0

struct ic_msg_hdr {
    uint32_t version_major;
    uint32_t version_minor;
    uint16_t msg_type;
    uint16_t msg_size;
    uint32_t status;
    uint8_t  transaction_id;
    uint8_t  flags;
    uint16_t reserved;
} __attribute__((packed));

struct ic_heartbeat {
    struct ic_msg_hdr hdr;
    uint64_t seq_num;
    uint32_t reserved[4];
} __attribute__((packed));

struct ic_shutdown {
    struct ic_msg_hdr hdr;
    uint32_t reason_code;
    uint32_t timeout_secs;
    uint32_t flags;
} __attribute__((packed));

static struct vmbus_channel *hb_ch;
static struct vmbus_channel *sd_ch;
static struct vmbus_channel *ts_ch;

static void hb_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[512];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len < (int)sizeof(struct ic_msg_hdr))
        return;

    struct ic_msg_hdr *hdr = (struct ic_msg_hdr *)buf;

    if (hdr->msg_type == ICMSG_NEGOTIATE) {
        hdr->flags = 4;
        hdr->status = 0;
        hdr->version_major = IC_VERSION_MAJOR;
        hdr->version_minor = IC_VERSION_MINOR;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    } else if (hdr->msg_type == ICMSG_HEARTBEAT) {
        hdr->flags = 4;
        hdr->status = 0;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    }
}

static void sd_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[512];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len < (int)sizeof(struct ic_msg_hdr))
        return;

    struct ic_msg_hdr *hdr = (struct ic_msg_hdr *)buf;

    if (hdr->msg_type == ICMSG_NEGOTIATE) {
        hdr->flags = 4;
        hdr->status = 0;
        hdr->version_major = IC_VERSION_MAJOR;
        hdr->version_minor = IC_VERSION_MINOR;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    } else if (hdr->msg_type == ICMSG_SHUTDOWN) {
        struct ic_shutdown *sd = (struct ic_shutdown *)buf;
        serial_puts("hv_utils: shutdown requested (");
        serial_puts(sd->flags ? "restart" : "halt");
        serial_puts(")\n");

        hdr->flags = 4;
        hdr->status = 0;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);

        serial_puts("hv_utils: halting...\n");
        __asm__ volatile("cli; hlt");
    }
}

static void ts_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[512];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len < (int)sizeof(struct ic_msg_hdr))
        return;

    struct ic_msg_hdr *hdr = (struct ic_msg_hdr *)buf;

    if (hdr->msg_type == ICMSG_NEGOTIATE) {
        hdr->flags = 4;
        hdr->status = 0;
        hdr->version_major = IC_VERSION_MAJOR;
        hdr->version_minor = IC_VERSION_MINOR;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    } else if (hdr->msg_type == ICMSG_TIMESYNC) {
        hdr->flags = 4;
        hdr->status = 0;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    }
}

static int open_util(const uint8_t guid[16], const char *name,
                     struct vmbus_channel **out,
                     void (*cb)(struct vmbus_channel *, void *)) {
    *out = vmbus_find_channel(guid);
    if (!*out) {
        serial_puts("hv_utils: ");
        serial_puts(name);
        serial_puts(" not found\n");
        return -1;
    }

    if (vmbus_open(*out, 16 * 1024, cb, 0) < 0) {
        serial_puts("hv_utils: ");
        serial_puts(name);
        serial_puts(" open failed\n");
        return -1;
    }

    serial_puts("hv_utils: ");
    serial_puts(name);
    serial_puts(" ready\n");
    return 0;
}

int hv_utils_init(void) {
    int ok = 0;
    if (open_util(HV_HEARTBEAT_GUID, "heartbeat", &hb_ch, hb_callback) == 0) ok++;
    if (open_util(HV_SHUTDOWN_GUID, "shutdown", &sd_ch, sd_callback) == 0) ok++;
    if (open_util(HV_TIMESYNC_GUID, "timesync", &ts_ch, ts_callback) == 0) ok++;

    serial_puts("hv_utils: ");
    serial_putchar('0' + ok);
    serial_puts("/3 services active\n");
    return ok > 0 ? 0 : -1;
}
