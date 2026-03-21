/* CosmoRT Framebuffer — glyph scaling + alpha blit
 *
 * Bilinear-scales font glyphs at boot (fixed-point, no float).
 * Alpha-blits individual glyphs for VT rendering.
 */
#ifndef FB_H
#define FB_H

#include <stdint.h>

struct boot_info;

/* Scaled glyph cache entry */
typedef struct {
    uint8_t *data;    /* alpha values, w * h bytes */
    int      w, h;    /* scaled dimensions */
} scaled_glyph_t;

/* Initialize framebuffer from boot info. Maps FB, scales glyphs. */
void fb_init(struct boot_info *info);

/* Blit a scaled glyph at pixel position (px, py) with fg/bg colors (0xRRGGBB). */
void fb_blit_glyph(int glyph_idx, int px, int py, uint32_t fg, uint32_t bg);

/* Fill a rectangle with a solid color. */
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Get scaled glyph dimensions (for VT grid sizing). */
int fb_glyph_w(void);
int fb_glyph_h(void);

/* Get framebuffer pixel dimensions. */
int fb_width(void);
int fb_height(void);

/* Is framebuffer available? */
int fb_available(void);

#endif
