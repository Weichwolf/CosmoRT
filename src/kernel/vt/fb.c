/* CosmoRT Framebuffer — bilinear glyph scaler + alpha blit
 *
 * All math is 16.16 fixed-point. No floating point in kernel.
 *
 * Boot: fb_init() maps the UEFI framebuffer, computes scale factor from
 * fb_height / 720, scales all font glyphs once via bilinear interpolation,
 * stores results in glyph_cache[].
 *
 * Render: fb_blit_glyph() alpha-composites a cached glyph onto the FB.
 */

#include "fb.h"
#include "boot_info.h"
#include "config.h"
#include "serial.h"
#include "page_alloc.h"
#include "memops.h"
#include "gen/font_atlas.h"

/* ── Fixed-point 16.16 ──────────────────────────────── */

#define FP_SHIFT 16
#define FP_ONE   (1 << FP_SHIFT)
#define FP_HALF  (1 << (FP_SHIFT - 1))

static inline int32_t fp_mul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> FP_SHIFT);
}

static inline int32_t fp_div(int32_t a, int32_t b) {
    if (b == 0) return 0;
    return (int32_t)(((int64_t)a << FP_SHIFT) / b);
}

/* ── State ──────────────────────────────────────────── */

static uint32_t *fb_base;
static int fb_w, fb_h, fb_pitch_px;
static int have_fb;

static scaled_glyph_t glyph_cache[FONT_NUM_GLYPHS];
static int scaled_w, scaled_h;

/* Bulk allocation for glyph data (avoid per-glyph page_alloc overhead) */
static uint8_t *glyph_data_pool;
static int glyph_data_offset;
static int glyph_data_capacity;

static uint8_t *glyph_data_alloc(int bytes) {
    /* Align to 8 bytes */
    bytes = (bytes + 7) & ~7;
    if (glyph_data_offset + bytes > glyph_data_capacity)
        return 0;
    uint8_t *p = glyph_data_pool + glyph_data_offset;
    glyph_data_offset += bytes;
    return p;
}

/* ── Bilinear interpolation (fixed-point) ──────────── */

static uint8_t bilinear_sample(const uint8_t *src, int src_w, int src_h,
                               int32_t sx_fp, int32_t sy_fp) {
    /* Clamp to source bounds */
    if (sx_fp < 0) sx_fp = 0;
    if (sy_fp < 0) sy_fp = 0;

    int x0 = sx_fp >> FP_SHIFT;
    int y0 = sy_fp >> FP_SHIFT;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x0 >= src_w) x0 = src_w - 1;
    if (x1 >= src_w) x1 = src_w - 1;
    if (y0 >= src_h) y0 = src_h - 1;
    if (y1 >= src_h) y1 = src_h - 1;

    /* Fractional parts (0..65535) */
    int32_t fx = sx_fp & (FP_ONE - 1);
    int32_t fy = sy_fp & (FP_ONE - 1);

    /* 4 source pixels */
    uint32_t p00 = src[y0 * src_w + x0];
    uint32_t p10 = src[y0 * src_w + x1];
    uint32_t p01 = src[y1 * src_w + x0];
    uint32_t p11 = src[y1 * src_w + x1];

    /* Bilinear blend (all in integer with 16-bit fraction) */
    uint32_t top = p00 * (uint32_t)(FP_ONE - fx) + p10 * (uint32_t)fx;
    uint32_t bot = p01 * (uint32_t)(FP_ONE - fx) + p11 * (uint32_t)fx;
    uint32_t val = (top >> FP_SHIFT) * (uint32_t)(FP_ONE - fy) +
                   (bot >> FP_SHIFT) * (uint32_t)fy;
    return (uint8_t)(val >> FP_SHIFT);
}

/* ── Scale all glyphs ──────────────────────────────── */

static void scale_all_glyphs(void) {
    int glyph_bytes = scaled_w * scaled_h;
    int total_bytes = glyph_bytes * FONT_NUM_GLYPHS;
    /* Round up to pages */
    int pages = (total_bytes + 4095) / 4096;
    glyph_data_pool = (uint8_t *)pages_alloc(pages);
    if (!glyph_data_pool) {
        serial_puts("fb: glyph alloc failed\n");
        return;
    }
    glyph_data_capacity = pages * 4096;
    glyph_data_offset = 0;

    /* Scale factors: source → dest mapping */
    int32_t sx_step = fp_div(FONT_BASE_W << FP_SHIFT, scaled_w << FP_SHIFT);
    int32_t sy_step = fp_div(FONT_BASE_H << FP_SHIFT, scaled_h << FP_SHIFT);

    int src_stride = FONT_BASE_W * FONT_BASE_H;

    for (int i = 0; i < FONT_NUM_GLYPHS; i++) {
        uint8_t *dst = glyph_data_alloc(glyph_bytes);
        if (!dst) break;
        glyph_cache[i].data = dst;
        glyph_cache[i].w = scaled_w;
        glyph_cache[i].h = scaled_h;

        const uint8_t *src = font_data + i * src_stride;

        for (int dy = 0; dy < scaled_h; dy++) {
            int32_t sy = fp_mul(dy << FP_SHIFT, sy_step);
            for (int dx = 0; dx < scaled_w; dx++) {
                int32_t sx = fp_mul(dx << FP_SHIFT, sx_step);
                dst[dy * scaled_w + dx] = bilinear_sample(src,
                    FONT_BASE_W, FONT_BASE_H, sx, sy);
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────── */

void fb_init(struct boot_info *info) {
    have_fb = 0;
    if (!info->fb_addr || !info->fb_width || !info->fb_height) {
        serial_puts("fb: no framebuffer\n");
        return;
    }

    fb_w = (int)info->fb_width;
    fb_h = (int)info->fb_height;
    fb_pitch_px = (int)(info->fb_pitch / (info->fb_bpp / 8));
    fb_base = (uint32_t *)phys_to_virt(info->fb_addr);

    /* Compute scaled glyph size: scale = fb_h / 720 (baseline) */
    /* For 1080p: scale = 1.5, glyph = 13x28 (from 9x19)
     * For 720p: scale = 1.0, glyph = 9x19
     * Fixed-point: scale_fp = fb_h * FP_ONE / 720 */
    int32_t scale_fp = fp_div(fb_h << FP_SHIFT, 720 << FP_SHIFT);
    /* Minimum scale = 1.0 */
    if (scale_fp < FP_ONE) scale_fp = FP_ONE;

    scaled_w = (fp_mul(FONT_BASE_W << FP_SHIFT, scale_fp) + FP_HALF) >> FP_SHIFT;
    scaled_h = (fp_mul(FONT_BASE_H << FP_SHIFT, scale_fp) + FP_HALF) >> FP_SHIFT;
    if (scaled_w < FONT_BASE_W) scaled_w = FONT_BASE_W;
    if (scaled_h < FONT_BASE_H) scaled_h = FONT_BASE_H;

    scale_all_glyphs();

    /* Clear to black */
    for (int y = 0; y < fb_h; y++)
        kmemset(fb_base + y * fb_pitch_px, 0, (size_t)fb_w * 4);

    have_fb = 1;

    serial_puts("fb: ");
    /* Print resolution */
    { int v = fb_w; char t[6]; int j = 0;
      do { t[j++] = '0' + (v % 10); v /= 10; } while (v);
      while (j--) serial_putchar(t[j]); }
    serial_putchar('x');
    { int v = fb_h; char t[6]; int j = 0;
      do { t[j++] = '0' + (v % 10); v /= 10; } while (v);
      while (j--) serial_putchar(t[j]); }
    serial_puts(" glyph ");
    { int v = scaled_w; char t[4]; int j = 0;
      do { t[j++] = '0' + (v % 10); v /= 10; } while (v);
      while (j--) serial_putchar(t[j]); }
    serial_putchar('x');
    { int v = scaled_h; char t[4]; int j = 0;
      do { t[j++] = '0' + (v % 10); v /= 10; } while (v);
      while (j--) serial_putchar(t[j]); }
    serial_putchar('\n');
}

void fb_blit_glyph(int glyph_idx, int px, int py, uint32_t fg, uint32_t bg) {
    if (!have_fb) return;
    if (glyph_idx < 0 || glyph_idx >= FONT_NUM_GLYPHS) glyph_idx = 0;
    scaled_glyph_t *g = &glyph_cache[glyph_idx];
    if (!g->data) return;

    uint8_t fg_r = (uint8_t)(fg >> 16), fg_g = (uint8_t)(fg >> 8), fg_b = (uint8_t)fg;
    uint8_t bg_r = (uint8_t)(bg >> 16), bg_g = (uint8_t)(bg >> 8), bg_b = (uint8_t)bg;

    for (int y = 0; y < g->h; y++) {
        int sy = py + y;
        if (sy < 0 || sy >= fb_h) continue;
        uint32_t *row = fb_base + sy * fb_pitch_px + px;
        for (int x = 0; x < g->w; x++) {
            if (px + x < 0 || px + x >= fb_w) continue;
            uint8_t a = g->data[y * g->w + x];
            if (a == 0) {
                row[x] = ((uint32_t)bg_r << 16) | ((uint32_t)bg_g << 8) | bg_b;
            } else if (a == 255) {
                row[x] = ((uint32_t)fg_r << 16) | ((uint32_t)fg_g << 8) | fg_b;
            } else {
                uint8_t inv = (uint8_t)(255 - a);
                uint8_t r = (uint8_t)((fg_r * a + bg_r * inv) / 255);
                uint8_t gb = (uint8_t)((fg_g * a + bg_g * inv) / 255);
                uint8_t b = (uint8_t)((fg_b * a + bg_b * inv) / 255);
                row[x] = ((uint32_t)r << 16) | ((uint32_t)gb << 8) | b;
            }
        }
    }
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!have_fb) return;
    for (int dy = 0; dy < h; dy++) {
        int sy = y + dy;
        if (sy < 0 || sy >= fb_h) continue;
        uint32_t *row = fb_base + sy * fb_pitch_px;
        for (int dx = 0; dx < w; dx++) {
            int sx = x + dx;
            if (sx >= 0 && sx < fb_w)
                row[sx] = color;
        }
    }
}

int fb_glyph_w(void) { return scaled_w; }
int fb_glyph_h(void) { return scaled_h; }
int fb_width(void)    { return fb_w; }
int fb_height(void)   { return fb_h; }
int fb_available(void) { return have_fb; }
