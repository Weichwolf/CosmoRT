/* CosmoRT hv_utils — Hyper-V Integration Services (Heartbeat, Shutdown, Time Sync)
 *
 * Responds to host heartbeat requests (keeps VM alive in Hyper-V manager).
 * Handles shutdown/restart requests for clean exit.
 *
 * Only imports: hw.h, config.h, serial.h, vmbus.h
 */

#include "vmbus.h"
#include "cosmo.h"

/* Hyper-V Heartbeat GUID: {57164f39-9115-4e78-ab55-382f3bd5422d} */
static const uint8_t HV_HEARTBEAT_GUID[16] = {
    0x39, 0x4f, 0x16, 0x57, 0x15, 0x91, 0x78, 0x4e,
    0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d
};

/* Hyper-V Shutdown GUID: {0e0b6031-5213-4934-818b-38d90ced39db} */
static const uint8_t HV_SHUTDOWN_GUID[16] = {
    0x31, 0x60, 0x0b, 0x0e, 0x13, 0x52, 0x34, 0x49,
    0x81, 0x8b, 0x38, 0xd9, 0x0c, 0xed, 0x39, 0xdb
};

/* Hyper-V Time Sync GUID: {9527e630-d0ae-497b-adce-e80ab0175caf} */
static const uint8_t HV_TIMESYNC_GUID[16] = {
    0x30, 0xe6, 0x27, 0x95, 0xae, 0xd0, 0x7b, 0x49,
    0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf
};

/* Integration service negotiation */
#define ICMSG_NEGOTIATE    0
#define ICMSG_HEARTBEAT    1
#define ICMSG_SHUTDOWN     3
#define ICMSG_TIMESYNC     4

#define IC_VERSION_MAJOR   3
#define IC_VERSION_MINOR   0

/* IC message header (common to all utils) */
struct ic_msg_hdr {
    uint32_t version_major;
    uint32_t version_minor;
    uint16_t msg_type;
    uint16_t msg_size;
    uint32_t status;
    uint8_t  transaction_id;
    uint8_t  flags;     /* bit 0 = transaction, bit 1 = request, bit 2 = response */
    uint16_t reserved;
} __attribute__((packed));

/* Heartbeat message */
struct ic_heartbeat {
    struct ic_msg_hdr hdr;
    uint64_t seq_num;
    uint32_t reserved[4];
} __attribute__((packed));

/* Shutdown message (truncated — display_msg can be large) */
struct ic_shutdown {
    struct ic_msg_hdr hdr;
    uint32_t reason_code;
    uint32_t timeout_secs;
    uint32_t flags;     /* 0 = shutdown, 1 = restart */
} __attribute__((packed));

/* ---- State ---- */

static struct vmbus_channel *hb_ch;
static struct vmbus_channel *sd_ch;
static struct vmbus_channel *ts_ch;

/* ---- Heartbeat callback ---- */

static void hb_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[512];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len < (int)sizeof(struct ic_msg_hdr))
        return;

    struct ic_msg_hdr *hdr = (struct ic_msg_hdr *)buf;

    if (hdr->msg_type == ICMSG_NEGOTIATE) {
        /* Respond with supported version */
        hdr->flags = 4;  /* response */
        hdr->status = 0;
        hdr->version_major = IC_VERSION_MAJOR;
        hdr->version_minor = IC_VERSION_MINOR;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    } else if (hdr->msg_type == ICMSG_HEARTBEAT) {
        /* Echo back as response */
        hdr->flags = 4;  /* response */
        hdr->status = 0;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    }
}

/* ---- Shutdown callback ---- */

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

        /* Acknowledge */
        hdr->flags = 4;
        hdr->status = 0;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);

        /* Halt */
        serial_puts("hv_utils: halting...\n");
        __asm__ volatile("cli; hlt");
    }
}

/* ---- Time sync callback ---- */

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
        /* Acknowledge time sync (we use our own TSC-based time) */
        hdr->flags = 4;
        hdr->status = 0;
        vmbus_send_pkt(ch, VMBUS_PKT_DATA_INBAND, buf, (size_t)len, 0, 0);
    }
}

/* ---- Init ---- */

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
