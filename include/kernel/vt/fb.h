/* CosmoRT Framebuffer — glyph scaling + alpha blit */
#ifndef FB_H
#define FB_H

#include <stdint.h>

struct boot_info;

typedef struct {
    uint8_t *data;
    int      w, h;
} scaled_glyph_t;

void fb_init(struct boot_info *info);

void fb_blit_glyph(int glyph_idx, int px, int py, uint32_t fg, uint32_t bg);

void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

int fb_glyph_w(void);
int fb_glyph_h(void);

int fb_width(void);
int fb_height(void);

int fb_available(void);

#endif
