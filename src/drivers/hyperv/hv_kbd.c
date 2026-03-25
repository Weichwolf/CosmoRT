/* CosmoRT hv_kbd — Hyper-V Synthetic Keyboard
 *
 * Receives keyboard input via VMBus channel.
 * Converts AT scancodes to HID and submits to input subsystem.
 *
 * Only imports: hw.h, config.h, serial.h, vmbus.h, input.h
 */

#include "vmbus.h"
#include "cosmort.h"

/* EV_KEY from Linux input-event-codes.h */
#define EV_KEY 0x01

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

/* AT (set 1) scancode → USB HID scancode */
static const uint8_t at_to_hid[128] = {
    [0x1E] = 0x04, /* a */ [0x30] = 0x05, /* b */ [0x2E] = 0x06, /* c */
    [0x20] = 0x07, /* d */ [0x12] = 0x08, /* e */ [0x21] = 0x09, /* f */
    [0x22] = 0x0A, /* g */ [0x23] = 0x0B, /* h */ [0x17] = 0x0C, /* i */
    [0x24] = 0x0D, /* j */ [0x25] = 0x0E, /* k */ [0x26] = 0x0F, /* l */
    [0x32] = 0x10, /* m */ [0x31] = 0x11, /* n */ [0x18] = 0x12, /* o */
    [0x19] = 0x13, /* p */ [0x10] = 0x14, /* q */ [0x13] = 0x15, /* r */
    [0x1F] = 0x16, /* s */ [0x14] = 0x17, /* t */ [0x16] = 0x18, /* u */
    [0x2F] = 0x19, /* v */ [0x11] = 0x1A, /* w */ [0x2D] = 0x1B, /* x */
    [0x15] = 0x1C, /* y */ [0x2C] = 0x1D, /* z */
    [0x02] = 0x1E, /* 1 */ [0x03] = 0x1F, /* 2 */ [0x04] = 0x20, /* 3 */
    [0x05] = 0x21, /* 4 */ [0x06] = 0x22, /* 5 */ [0x07] = 0x23, /* 6 */
    [0x08] = 0x24, /* 7 */ [0x09] = 0x25, /* 8 */ [0x0A] = 0x26, /* 9 */
    [0x0B] = 0x27, /* 0 */
    [0x1C] = 0x28, /* Enter */  [0x01] = 0x29, /* Escape */
    [0x0E] = 0x2A, /* Backspace */ [0x0F] = 0x2B, /* Tab */
    [0x39] = 0x2C, /* Space */
    [0x0C] = 0x2D, /* - */  [0x0D] = 0x2E, /* = */
    [0x1A] = 0x2F, /* [ */  [0x1B] = 0x30, /* ] */
    [0x2B] = 0x31, /* \ */  [0x27] = 0x33, /* ; */
    [0x28] = 0x34, /* ' */  [0x29] = 0x35, /* ` */
    [0x33] = 0x36, /* , */  [0x34] = 0x37, /* . */
    [0x35] = 0x38, /* / */
    /* Modifier scancodes (AT set 1) */
    [0x2A] = 0xE1, /* LShift */ [0x36] = 0xE5, /* RShift */
    [0x1D] = 0xE0, /* LCtrl */  [0x38] = 0xE2, /* LAlt */
    /* F1-F4 for VT switching */
    [0x3B] = 0x3A, /* F1 */ [0x3C] = 0x3B, /* F2 */
    [0x3D] = 0x3C, /* F3 */ [0x3E] = 0x3D, /* F4 */
};

/* ---- State ---- */

static struct vmbus_channel *kbd_ch;

/* ---- Input subsystem registration ---- */

static const input_driver_t hv_kbd_driver = {
    .name = "hv-kbd",
};

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
            /* Convert AT scancode to HID and submit to input subsystem */
            if (key->make_code < 128) {
                uint8_t hid = at_to_hid[key->make_code];
                if (hid != 0) {
                    input_event_t ev = { EV_KEY, hid, up ? 0 : 1 };
                    input_submit_event(&ev);
                }
            }
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

    input_register(&hv_kbd_driver);
    serial_puts("hv_kbd: ready\n");
    return 0;
}
