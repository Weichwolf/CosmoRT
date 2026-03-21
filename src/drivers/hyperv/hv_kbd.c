/* CosmoRT hv_kbd — Hyper-V Synthetic Keyboard
 *
 * Receives keyboard input via VMBus channel.
 * Stub: detects and opens channel, logs keypresses.
 *
 * Only imports: hw.h, config.h, serial.h, vmbus.h
 */

#include "vmbus.h"
#include "hw.h"
#include "config.h"
#include "serial.h"

/* Hyper-V Keyboard GUID: {f912ad6d-2b17-48ea-bd65-f927a61c7684} */
static const uint8_t HV_KBD_GUID[16] = {
    0x6d, 0xad, 0x12, 0xf9, 0x17, 0x2b, 0xea, 0x48,
    0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84
};

/* Keyboard protocol */
#define HV_KBD_PROTO_VERSION_REQUEST  1
#define HV_KBD_PROTO_VERSION_RESPONSE 2
#define HV_KBD_PROTO_EVENT            3

struct hv_kbd_msg {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct hv_kbd_version_req {
    struct hv_kbd_msg hdr;
    uint32_t version;
} __attribute__((packed));

struct hv_kbd_keystroke {
    struct hv_kbd_msg hdr;
    uint16_t make_code;
    uint16_t reserved;
    uint32_t info;  /* bit 0: key_up, bit 1: unicode, bit 2: e0, bit 3: e1 */
} __attribute__((packed));

/* ---- State ---- */

static struct vmbus_channel *kbd_ch;

/* ---- Callback ---- */

static void hv_kbd_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[256];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len >= (int)sizeof(struct hv_kbd_msg)) {
        struct hv_kbd_msg *msg = (struct hv_kbd_msg *)buf;
        if (msg->type == HV_KBD_PROTO_EVENT && len >= (int)sizeof(struct hv_kbd_keystroke)) {
            struct hv_kbd_keystroke *key = (struct hv_kbd_keystroke *)buf;
            int up = key->info & 1;
            (void)up;
            /* Key events available for input subsystem integration */
        }
    }
}

/* ---- Init ---- */

int hv_kbd_init(void) {
    kbd_ch = vmbus_find_channel(HV_KBD_GUID);
    if (!kbd_ch) {
        serial_puts("hv_kbd: no channel found\n");
        return -1;
    }
    serial_puts("hv_kbd: channel found, relid=");
    serial_hex64(kbd_ch->child_relid);
    serial_putchar('\n');

    if (vmbus_open(kbd_ch, 16 * 1024, hv_kbd_callback, 0) < 0) {
        serial_puts("hv_kbd: open failed\n");
        return -1;
    }

    /* Version negotiation */
    struct hv_kbd_version_req ver;
    hw_memset(&ver, 0, sizeof(ver));
    ver.hdr.type = HV_KBD_PROTO_VERSION_REQUEST;
    ver.hdr.size = sizeof(ver);
    ver.version = 1;

    vmbus_send_pkt(kbd_ch, VMBUS_PKT_DATA_INBAND, &ver, sizeof(ver), 0, 0);

    serial_puts("hv_kbd: ready (stub)\n");
    return 0;
}
