/* Boot info structure — shared between bootloader and kernel */
#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

struct boot_info {
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint64_t mmap_addr;
    uint64_t mmap_size;
    uint64_t mmap_desc_size;
    uint64_t rsdp_addr;
};

#endif
