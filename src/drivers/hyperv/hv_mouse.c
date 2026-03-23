/* CosmoRT hv_mouse — Hyper-V Synthetic Mouse
 *
 * Receives mouse input via VMBus channel (Synthrdp HID).
 * Stub: detects and opens channel.
 *
 * Only imports: hw.h, config.h, serial.h, vmbus.h
 */

#include "vmbus.h"
#include "cosmo_hw.h"

/* Hyper-V Mouse GUID: {cfa8b69e-5b4a-4cc0-b98b-8ba1a1f3f95a} */
static const uint8_t HV_MOUSE_GUID[16] = {
    0x9e, 0xb6, 0xa8, 0xcf, 0x4a, 0x5b, 0xc0, 0x4c,
    0xb9, 0x8b, 0x8b, 0xa1, 0xa1, 0xf3, 0xf9, 0x5a
};

/* Mouse protocol */
#define HV_MOUSE_PROTO_REQUEST   1
#define HV_MOUSE_PROTO_RESPONSE  2
#define HV_MOUSE_PROTO_EVENT     3
#define HV_MOUSE_PROTO_ACK       4

struct hv_mouse_msg {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct hv_mouse_version_req {
    struct hv_mouse_msg hdr;
    uint32_t version;
} __attribute__((packed));

struct hv_mouse_report {
    struct hv_mouse_msg hdr;
    uint8_t  report_data[];  /* HID report */
} __attribute__((packed));

/* ---- State ---- */

static struct vmbus_channel *mouse_ch;

/* ---- Callback ---- */

static void hv_mouse_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[256];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len >= (int)sizeof(struct hv_mouse_msg)) {
        struct hv_mouse_msg *msg = (struct hv_mouse_msg *)buf;
        if (msg->type == HV_MOUSE_PROTO_EVENT) {
            /* Mouse events available for input subsystem integration */
        }
    }
}

/* ---- Init ---- */

int hv_mouse_init(void) {
    mouse_ch = vmbus_find_channel(HV_MOUSE_GUID);
    if (!mouse_ch) {
        serial_puts("hv_mouse: no channel found\n");
        return -1;
    }
    serial_puts("hv_mouse: channel found, relid=");
    serial_hex64(mouse_ch->child_relid);
    serial_putchar('\n');

    if (vmbus_open(mouse_ch, 16 * 1024, hv_mouse_callback, 0) < 0) {
        serial_puts("hv_mouse: open failed\n");
        return -1;
    }

    /* Version negotiation */
    struct hv_mouse_version_req ver;
    hw_memset(&ver, 0, sizeof(ver));
    ver.hdr.type = HV_MOUSE_PROTO_REQUEST;
    ver.hdr.size = sizeof(ver);
    ver.version = 1;

    vmbus_send_pkt(mouse_ch, VMBUS_PKT_DATA_INBAND, &ver, sizeof(ver), 0, 0);

    serial_puts("hv_mouse: ready (stub)\n");
    return 0;
}
