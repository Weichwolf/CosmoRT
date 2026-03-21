/* CosmoRT hyperv_fb — Hyper-V Synthetic Framebuffer (Synthvid)
 *
 * Maps the Hyper-V framebuffer for display output.
 * Stub: detects device and reports resolution. Full rendering deferred.
 *
 * Only imports: hw.h, config.h, serial.h, vmbus.h
 */

#include "vmbus.h"
#include "hw.h"
#include "config.h"
#include "serial.h"

/* Hyper-V Synthetic Video GUID: {da0a7802-e377-4aac-8e77-0558eb1073f8} */
static const uint8_t SYNTHVID_GUID[16] = {
    0x02, 0x78, 0x0a, 0xda, 0x77, 0xe3, 0xac, 0x4a,
    0x8e, 0x77, 0x05, 0x58, 0xeb, 0x10, 0x73, 0xf8
};

/* Synthvid protocol versions */
#define SYNTHVID_VERSION_WIN10  0x00030005

/* Synthvid message types */
#define SYNTHVID_MSG_ERROR             0
#define SYNTHVID_MSG_VERSION_REQUEST   1
#define SYNTHVID_MSG_VERSION_RESPONSE  2
#define SYNTHVID_MSG_VRAM_LOCATION     3
#define SYNTHVID_MSG_VRAM_LOCATION_ACK 4
#define SYNTHVID_MSG_SITUATION_UPDATE  5
#define SYNTHVID_MSG_SITUATION_UPDATE_ACK 6
#define SYNTHVID_MSG_POINTER_POSITION  7
#define SYNTHVID_MSG_POINTER_SHAPE     8
#define SYNTHVID_MSG_FEATURE_CHANGE    9
#define SYNTHVID_MSG_DIRT              10

struct synthvid_msg_hdr {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct synthvid_version_req {
    struct synthvid_msg_hdr hdr;
    uint32_t version;
} __attribute__((packed));

struct synthvid_version_resp {
    struct synthvid_msg_hdr hdr;
    uint32_t version;
    uint8_t  is_accepted;
    uint8_t  max_video_outputs;
    uint8_t  padding[2];
} __attribute__((packed));

struct synthvid_vram_location {
    struct synthvid_msg_hdr hdr;
    uint64_t user_ctx;
    uint8_t  is_vram_gpa_specified;
    uint8_t  padding[7];
    uint64_t vram_gpa;
} __attribute__((packed));

/* ---- State ---- */

static struct vmbus_channel *fb_ch;
static volatile int fb_resp_ready;

/* ---- Callback ---- */

static void hyperv_fb_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ch; (void)ctx;
    fb_resp_ready = 1;
}

/* ---- Init ---- */

int hyperv_fb_init(void) {
    fb_ch = vmbus_find_channel(SYNTHVID_GUID);
    if (!fb_ch) {
        serial_puts("hyperv_fb: no channel found\n");
        return -1;
    }
    serial_puts("hyperv_fb: channel found, relid=");
    serial_hex64(fb_ch->child_relid);
    serial_putchar('\n');

    if (vmbus_open(fb_ch, 32 * 1024, hyperv_fb_callback, 0) < 0) {
        serial_puts("hyperv_fb: open failed\n");
        return -1;
    }

    /* Version negotiation */
    struct synthvid_version_req ver;
    hw_memset(&ver, 0, sizeof(ver));
    ver.hdr.type = SYNTHVID_MSG_VERSION_REQUEST;
    ver.hdr.size = sizeof(ver);
    ver.version = SYNTHVID_VERSION_WIN10;

    fb_resp_ready = 0;
    vmbus_send_pkt(fb_ch, VMBUS_PKT_DATA_INBAND, &ver, sizeof(ver), 0, 0);

    uint64_t deadline = hw_ms() + 3000;
    while (!fb_resp_ready && hw_ms() < deadline)
        __asm__ volatile("pause");

    serial_puts("hyperv_fb: ready (stub)\n");
    return 0;
}
