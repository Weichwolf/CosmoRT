/* CosmoRT virtio-gpu driver — 2D framebuffer
 *
 * virtio-gpu PCI: vendor 0x1AF4, device 0x1050 (modern).
 * Also matches legacy device 0x1000-0x103F with subsys 16.
 * VQ 0: controlq (commands), VQ 1: cursorq (unused).
 *
 * Minimal 2D pipeline:
 *   GET_DISPLAY_INFO -> RESOURCE_CREATE_2D -> RESOURCE_ATTACH_BACKING
 *   -> SET_SCANOUT -> TRANSFER_TO_HOST_2D -> RESOURCE_FLUSH
 */

#include "virtio_gpu.h"
#include "virtio.h"
#include "cosmo.h"

/* ── Virtio-GPU command types ──────────────────────── */

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF        0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT           0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH        0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO      0x1101

/* Pixel format: B8G8R8X8 (32-bit BGRX, no alpha) */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM     2

/* ── Command/response structures ───────────────────── */

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by nr_entries × virtio_gpu_mem_entry */
} __attribute__((packed));

/* ── Driver state ──────────────────────────────────── */

static virtio_dev_t gpu_dev;
static virtqueue_t  ctrlq;
static int initialized;

static uint32_t disp_width, disp_height;
static uint32_t *framebuf;        /* kernel-virtual pointer to FB */
static uint64_t framebuf_phys;
static uint32_t framebuf_size;

/* DMA command/response buffer */
static uint8_t *cmd_buf;
static uint64_t cmd_buf_phys;
#define CMD_BUF_SIZE 4096

static hw_spinlock_t gpu_lock = HW_SPINLOCK_INIT;

#define RESOURCE_ID 1

/* ── Send command + receive response ───────────────── */

static int gpu_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len) {
    /* Copy command to DMA buffer */
    hw_memcpy(cmd_buf, cmd, cmd_len);
    hw_memset(cmd_buf + cmd_len, 0, resp_len);

    /* Need 2 descriptors: cmd (out) + response (in) */
    int head = virtqueue_alloc_descs(&ctrlq, 2);
    if (head < 0) return -1;

    uint16_t d0 = (uint16_t)head;
    ctrlq.desc[d0].addr  = cmd_buf_phys;
    ctrlq.desc[d0].len   = cmd_len;
    ctrlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    uint16_t d1 = ctrlq.desc[d0].next;

    ctrlq.desc[d1].addr  = cmd_buf_phys + cmd_len;
    ctrlq.desc[d1].len   = resp_len;
    ctrlq.desc[d1].flags = VIRTQ_DESC_F_WRITE;

    virtqueue_submit(&ctrlq, (uint16_t)head);
    virtqueue_kick(&gpu_dev, 0);

    /* Poll for completion */
    uint64_t deadline = hw_ms() + 2000;
    while (virtqueue_get_used(&ctrlq, 0) < 0) {
        if (hw_ms() > deadline) {
            virtqueue_free_chain(&ctrlq, (uint16_t)head);
            return -1;
        }
        __asm__ volatile("pause");
    }
    virtqueue_free_chain(&ctrlq, (uint16_t)head);

    /* Copy response back */
    if (resp)
        hw_memcpy(resp, cmd_buf + cmd_len, resp_len);

    return 0;
}

/* ── Init ──────────────────────────────────────────── */

int virtio_gpu_init(void) {
    /* PCI scan: virtio-gpu = device 0x1050 (modern) or subsys 16 (legacy) */
    if (virtio_pci_find(0x1050, 16, &gpu_dev) < 0) {
        serial_puts("virtio-gpu: not found\n");
        return -1;
    }
    serial_puts("virtio-gpu: found on PCI\n");

    virtio_dev_init(&gpu_dev, 0);  /* no special features needed */

    /* Setup controlq (VQ 0) */
    if (virtqueue_setup(&gpu_dev, 0, &ctrlq) < 0) {
        serial_puts("virtio-gpu: VQ setup failed\n");
        return -1;
    }

    /* Allocate command DMA buffer */
    void *dma_virt;
    uint64_t dma_phys;
    if (cosmo_dma_alloc(CMD_BUF_SIZE, &dma_virt, &dma_phys) < 0) {
        serial_puts("virtio-gpu: cmd DMA alloc failed\n");
        return -1;
    }
    cmd_buf      = (uint8_t *)dma_virt;
    cmd_buf_phys = dma_phys;
    hw_memset(cmd_buf, 0, CMD_BUF_SIZE);

    virtio_set_driver_ok(&gpu_dev);

    /* 1. Get display info */
    struct virtio_gpu_ctrl_hdr get_info = { .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO };
    struct virtio_gpu_resp_display_info info;
    if (gpu_cmd(&get_info, sizeof(get_info), &info, sizeof(info)) < 0 ||
        info.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        serial_puts("virtio-gpu: GET_DISPLAY_INFO failed\n");
        return -1;
    }

    /* Find first enabled display */
    disp_width = disp_height = 0;
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (info.pmodes[i].enabled && info.pmodes[i].r.width > 0) {
            disp_width  = info.pmodes[i].r.width;
            disp_height = info.pmodes[i].r.height;
            break;
        }
    }
    if (disp_width == 0) {
        /* Default fallback */
        disp_width  = 1024;
        disp_height = 768;
    }

    serial_puts("virtio-gpu: ");
    { char t[8]; int i=0; uint32_t v=disp_width;
      do{t[i++]='0'+(char)(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); }
    serial_putchar('x');
    { char t[8]; int i=0; uint32_t v=disp_height;
      do{t[i++]='0'+(char)(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); }
    serial_putchar('\n');

    /* Allocate framebuffer (4 bytes/pixel, BGRX) */
    framebuf_size = disp_width * disp_height * 4;
    /* Round up to page boundary */
    uint32_t alloc_size = (framebuf_size + 4095) & ~4095U;
    void *fb_virt;
    if (cosmo_dma_alloc(alloc_size, &fb_virt, &framebuf_phys) < 0) {
        serial_puts("virtio-gpu: FB alloc failed\n");
        return -1;
    }
    framebuf = (uint32_t *)fb_virt;
    hw_memset(framebuf, 0, framebuf_size);

    /* 2. Create 2D resource */
    struct virtio_gpu_resource_create_2d create = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D },
        .resource_id = RESOURCE_ID,
        .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width  = disp_width,
        .height = disp_height
    };
    struct virtio_gpu_ctrl_hdr resp;
    if (gpu_cmd(&create, sizeof(create), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("virtio-gpu: RESOURCE_CREATE_2D failed\n");
        return -1;
    }

    /* 3. Attach backing storage — send header + 1 mem_entry as single command */
    struct {
        struct virtio_gpu_resource_attach_backing hdr;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) attach = {
        .hdr = {
            .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING },
            .resource_id = RESOURCE_ID,
            .nr_entries  = 1
        },
        .entry = {
            .addr   = framebuf_phys,
            .length = framebuf_size,
            .padding = 0
        }
    };
    if (gpu_cmd(&attach, sizeof(attach), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("virtio-gpu: ATTACH_BACKING failed\n");
        return -1;
    }

    /* 4. Set scanout */
    struct virtio_gpu_set_scanout scanout = {
        .hdr = { .type = VIRTIO_GPU_CMD_SET_SCANOUT },
        .r = { .x = 0, .y = 0, .width = disp_width, .height = disp_height },
        .scanout_id = 0,
        .resource_id = RESOURCE_ID
    };
    if (gpu_cmd(&scanout, sizeof(scanout), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("virtio-gpu: SET_SCANOUT failed\n");
        return -1;
    }

    initialized = 1;
    serial_puts("virtio-gpu: ready\n");
    return 0;
}

/* ── Framebuffer access ────────────────────────────── */

void *virtio_gpu_framebuffer(uint32_t *width, uint32_t *height, uint32_t *pitch) {
    if (!initialized) return 0;
    if (width)  *width  = disp_width;
    if (height) *height = disp_height;
    if (pitch)  *pitch  = disp_width * 4;
    return framebuf;
}

/* ── Flush dirty region to display ─────────────────── */

void virtio_gpu_flush(int x, int y, int w, int h) {
    if (!initialized) return;

    /* Clamp to display bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)disp_width)  w = (int)disp_width  - x;
    if (y + h > (int)disp_height) h = (int)disp_height - y;
    if (w <= 0 || h <= 0) return;

    uint64_t flags;
    hw_spin_lock_irq(&gpu_lock, &flags);

    /* Transfer to host */
    struct virtio_gpu_transfer_to_host_2d xfer = {
        .hdr = { .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D },
        .r = { .x = (uint32_t)x, .y = (uint32_t)y,
               .width = (uint32_t)w, .height = (uint32_t)h },
        .offset = 0,
        .resource_id = RESOURCE_ID
    };
    struct virtio_gpu_ctrl_hdr resp;
    gpu_cmd(&xfer, sizeof(xfer), &resp, sizeof(resp));

    /* Flush (make visible) */
    struct virtio_gpu_resource_flush flush = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH },
        .r = { .x = (uint32_t)x, .y = (uint32_t)y,
               .width = (uint32_t)w, .height = (uint32_t)h },
        .resource_id = RESOURCE_ID
    };
    gpu_cmd(&flush, sizeof(flush), &resp, sizeof(resp));

    hw_spin_unlock_irq(&gpu_lock, flags);
}
