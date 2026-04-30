/* QEMU fw_cfg PIO reader.
 *
 * QEMU exposes config blobs via two ports:
 *   0x510 (W16): selector
 *   0x511 (R8) : data, advances cursor on each read
 *
 * Standard keys we use:
 *   0x0000  signature ("QEMU" — 4 bytes)
 *   0x0019  file directory (count BE32, then entries)
 *
 * File-directory entries are 64 bytes each:
 *   uint32 size   (BE)
 *   uint16 select (BE)  — selector key for this file's data
 *   uint16 reserved
 *   char   name[56]
 *
 * Spec: https://www.qemu.org/docs/master/specs/fw_cfg.html */

#include "hw/fw_cfg.h"
#include "hal/hal.h"

#define FW_CFG_PORT_SEL  0x510
#define FW_CFG_PORT_DATA 0x511

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

static int g_probed;
static int g_present;

static void fw_cfg_select(uint16_t key) {
    hal_io_outw(FW_CFG_PORT_SEL, key);
}

static void fw_cfg_read(void *buf, int len) {
    uint8_t *p = (uint8_t *)buf;
    for (int i = 0; i < len; i++)
        p[i] = hal_io_inb(FW_CFG_PORT_DATA);
}

static uint32_t be32_to_cpu(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
           ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
}

static uint16_t be16_to_cpu(uint16_t v) {
    return (uint16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
}

static int kstreq_local(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int fw_cfg_probe(void) {
    if (g_probed) return g_present;
    g_probed = 1;
    fw_cfg_select(FW_CFG_SIGNATURE);
    char sig[4];
    fw_cfg_read(sig, 4);
    g_present = (sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U');
    return g_present;
}

int fw_cfg_read_file(const char *name, char *buf, int bufsize) {
    if (!fw_cfg_probe()) return 0;
    if (!buf || bufsize <= 0) return -1;

    fw_cfg_select(FW_CFG_FILE_DIR);
    uint32_t count_be;
    fw_cfg_read(&count_be, 4);
    uint32_t count = be32_to_cpu(count_be);
    /* Sanity-bound: real QEMU exposes ~30-100 entries; cap to avoid runaway. */
    if (count > 4096) return -1;

    for (uint32_t i = 0; i < count; i++) {
        struct {
            uint32_t size;
            uint16_t select;
            uint16_t reserved;
            char     name[56];
        } entry;
        fw_cfg_read(&entry, sizeof(entry));
        entry.name[55] = 0;
        if (!kstreq_local(entry.name, name)) continue;

        uint32_t size = be32_to_cpu(entry.size);
        uint16_t sel = be16_to_cpu(entry.select);
        if (size == 0) { buf[0] = 0; return 0; }

        int copy = (int)size;
        if (copy > bufsize - 1) copy = bufsize - 1;
        fw_cfg_select(sel);
        fw_cfg_read(buf, copy);
        buf[copy] = 0;
        return copy;
    }
    return 0;
}
