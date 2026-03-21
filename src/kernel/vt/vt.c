/* CosmoRT Virtual Terminal — ANSI parser + framebuffer renderer
 *
 * Each VT: character grid (Unicode codepoints + color attrs),
 * ANSI escape state machine, UTF-8 decoder, associated PTY.
 *
 * Rendering: dirty-line tracking, only re-renders changed rows.
 * Font: glyph_cache from fb.c (bilinear-scaled at boot).
 */

#include "vt.h"
#include "pty.h"
#include "fb.h"
#include "serial.h"
#include "memops.h"
#include "page_alloc.h"
#include "gen/font_atlas.h"

#define VT_MAX 4

/* ── ANSI 16-color palette ─────────────────────────── */

/* BMW Bernstein — warm amber on black, like classic instrument clusters */
static const uint32_t ansi_colors[16] = {
    0x000000, 0xCC3300, 0x669900, 0xCC8800,  /* black, red, green, brown */
    0x336699, 0x994499, 0x339999, 0xFF8C00,  /* blue, magenta, cyan, DEFAULT (amber) */
    0x663300, 0xFF6633, 0x99CC33, 0xFFCC33,  /* bright: dim, orange, lime, gold */
    0x6699CC, 0xCC66CC, 0x66CCCC, 0xFFAA33,  /* bright: blue, magenta, cyan, bright amber */
};

/* ── Per-VT state ──────────────────────────────────── */

typedef struct {
    uint32_t *chars;        /* Unicode codepoints (cols * rows) */
    uint8_t  *attrs;        /* color: (bg << 4) | fg */
    int       cols, rows;

    int       cursor_x, cursor_y;
    int       cursor_visible;

    /* ANSI parser */
    int       esc_state;    /* 0=normal, 1=ESC, 2=CSI params, 3=OSC */
    int       esc_params[8];
    int       esc_nparam;
    int       esc_qmark;    /* CSI ? prefix */

    /* Current attributes */
    uint8_t   fg_color;     /* 0-15 */
    uint8_t   bg_color;     /* 0-15 */
    int       bold;

    /* UTF-8 decoder */
    int       utf8_remaining;
    uint32_t  utf8_codepoint;

    /* Associated PTY */
    int       pty_id;

    /* Dirty tracking */
    int       dirty_top;
    int       dirty_bottom;  /* exclusive */
} vt_t;

static vt_t vts[VT_MAX];
static int active_vt;
static int grid_cols, grid_rows;

/* ── Keyboard state ────────────────────────────────── */

static int key_shift;
static int key_ctrl;
static int key_alt;

/* ── Linux KEY_* code → ASCII (US layout) ──────────── */
/* virtio-input sends Linux input event codes, NOT USB HID scancodes.
 * KEY_A=30, KEY_B=48, etc. (AT scancode order). */

static const uint8_t keymap_normal[256] = {
    [2]='1', [3]='2', [4]='3', [5]='4', [6]='5', [7]='6', [8]='7', [9]='8',
    [10]='9', [11]='0', [12]='-', [13]='=', [14]='\b', [15]='\t',
    [16]='q', [17]='w', [18]='e', [19]='r', [20]='t', [21]='y', [22]='u',
    [23]='i', [24]='o', [25]='p', [26]='[', [27]=']', [28]='\n',
    [30]='a', [31]='s', [32]='d', [33]='f', [34]='g', [35]='h', [36]='j',
    [37]='k', [38]='l', [39]=';', [40]='\'', [41]='`',
    [43]='\\',
    [44]='z', [45]='x', [46]='c', [47]='v', [48]='b', [49]='n', [50]='m',
    [51]=',', [52]='.', [53]='/',
    [57]=' ',
    [1]=0x1B, /* Escape */
};

static const uint8_t keymap_shifted[256] = {
    [2]='!', [3]='@', [4]='#', [5]='$', [6]='%', [7]='^', [8]='&', [9]='*',
    [10]='(', [11]=')', [12]='_', [13]='+', [14]='\b', [15]='\t',
    [16]='Q', [17]='W', [18]='E', [19]='R', [20]='T', [21]='Y', [22]='U',
    [23]='I', [24]='O', [25]='P', [26]='{', [27]='}', [28]='\n',
    [30]='A', [31]='S', [32]='D', [33]='F', [34]='G', [35]='H', [36]='J',
    [37]='K', [38]='L', [39]=':', [40]='"', [41]='~',
    [43]='|',
    [44]='Z', [45]='X', [46]='C', [47]='V', [48]='B', [49]='N', [50]='M',
    [51]='<', [52]='>', [53]='?',
    [57]=' ',
    [1]=0x1B,
};

/* Linux KEY_* modifier codes */
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTCTRL   29
#define KEY_RIGHTCTRL  97
#define KEY_LEFTALT    56
#define KEY_RIGHTALT   100
#define KEY_F1         59
#define KEY_F4         62

/* ── Codepoint → glyph index (binary search on font_map) ── */

static int cp_to_glyph(uint32_t cp) {
    int lo = 0, hi = (int)FONT_MAP_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_map[mid].codepoint == cp) return font_map[mid].index;
        if (font_map[mid].codepoint < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0; /* space */
}

/* ── Dirty tracking ────────────────────────────────── */

static void mark_dirty(vt_t *vt, int row) {
    if (row < vt->dirty_top) vt->dirty_top = row;
    if (row + 1 > vt->dirty_bottom) vt->dirty_bottom = row + 1;
}

static void mark_all_dirty(vt_t *vt) {
    vt->dirty_top = 0;
    vt->dirty_bottom = vt->rows;
}

/* ── Cell access ───────────────────────────────────── */

static int cell_idx(vt_t *vt, int x, int y) {
    return y * vt->cols + x;
}

static void set_cell(vt_t *vt, int x, int y, uint32_t cp, uint8_t attr) {
    int idx = cell_idx(vt, x, y);
    vt->chars[idx] = cp;
    vt->attrs[idx] = attr;
    mark_dirty(vt, y);
}

static uint8_t make_attr(vt_t *vt) {
    uint8_t fg = vt->fg_color;
    if (vt->bold && fg < 8) fg += 8;  /* bold = bright */
    return (uint8_t)((vt->bg_color << 4) | fg);
}

/* ── Scrolling ─────────────────────────────────────── */

static void scroll_up(vt_t *vt, int lines) {
    if (lines <= 0) return;
    if (lines >= vt->rows) {
        /* Clear entire screen */
        kmemset(vt->chars, 0, (size_t)(vt->cols * vt->rows) * sizeof(uint32_t));
        kmemset(vt->attrs, 0, (size_t)(vt->cols * vt->rows));
        mark_all_dirty(vt);
        return;
    }
    int move_rows = vt->rows - lines;
    /* Move character data up */
    kmemcpy(vt->chars, vt->chars + lines * vt->cols,
            (size_t)(move_rows * vt->cols) * sizeof(uint32_t));
    kmemcpy(vt->attrs, vt->attrs + lines * vt->cols,
            (size_t)(move_rows * vt->cols));
    /* Clear bottom lines */
    kmemset(vt->chars + move_rows * vt->cols, 0,
            (size_t)(lines * vt->cols) * sizeof(uint32_t));
    kmemset(vt->attrs + move_rows * vt->cols, 0,
            (size_t)(lines * vt->cols));
    mark_all_dirty(vt);
}

/* ── Cursor advance ────────────────────────────────── */

static void cursor_advance(vt_t *vt) {
    vt->cursor_x++;
    if (vt->cursor_x >= vt->cols) {
        vt->cursor_x = 0;
        vt->cursor_y++;
        if (vt->cursor_y >= vt->rows) {
            scroll_up(vt, 1);
            vt->cursor_y = vt->rows - 1;
        }
    }
}

static void cursor_newline(vt_t *vt) {
    vt->cursor_x = 0;
    vt->cursor_y++;
    if (vt->cursor_y >= vt->rows) {
        scroll_up(vt, 1);
        vt->cursor_y = vt->rows - 1;
    }
}

/* ── ANSI SGR (Select Graphic Rendition) ───────────── */

static void apply_sgr(vt_t *vt, int param) {
    if (param == 0) {
        vt->fg_color = 7; vt->bg_color = 0; vt->bold = 0;
    } else if (param == 1) {
        vt->bold = 1;
    } else if (param == 22) {
        vt->bold = 0;
    } else if (param >= 30 && param <= 37) {
        vt->fg_color = (uint8_t)(param - 30);
    } else if (param == 39) {
        vt->fg_color = 7; /* default fg */
    } else if (param >= 40 && param <= 47) {
        vt->bg_color = (uint8_t)(param - 40);
    } else if (param == 49) {
        vt->bg_color = 0; /* default bg */
    } else if (param >= 90 && param <= 97) {
        vt->fg_color = (uint8_t)(param - 90 + 8); /* bright fg */
    } else if (param >= 100 && param <= 107) {
        vt->bg_color = (uint8_t)(param - 100 + 8); /* bright bg */
    } else if (param == 7) {
        /* Reverse video: swap fg/bg */
        uint8_t tmp = vt->fg_color;
        vt->fg_color = vt->bg_color;
        vt->bg_color = tmp;
    } else if (param == 27) {
        /* Reverse off — just reset to defaults (simplification) */
        vt->fg_color = 7; vt->bg_color = 0;
    }
}

/* ── ANSI CSI dispatch ─────────────────────────────── */

static void csi_dispatch(vt_t *vt, char cmd) {
    int p0 = vt->esc_nparam > 0 ? vt->esc_params[0] : 0;
    int p1 = vt->esc_nparam > 1 ? vt->esc_params[1] : 0;
    int n = p0 ? p0 : 1; /* default 1 for movement commands */

    switch (cmd) {
    case 'A': /* Cursor up */
        vt->cursor_y -= n;
        if (vt->cursor_y < 0) vt->cursor_y = 0;
        break;
    case 'B': /* Cursor down */
        vt->cursor_y += n;
        if (vt->cursor_y >= vt->rows) vt->cursor_y = vt->rows - 1;
        break;
    case 'C': /* Cursor forward */
        vt->cursor_x += n;
        if (vt->cursor_x >= vt->cols) vt->cursor_x = vt->cols - 1;
        break;
    case 'D': /* Cursor back */
        vt->cursor_x -= n;
        if (vt->cursor_x < 0) vt->cursor_x = 0;
        break;
    case 'E': /* Cursor next line */
        vt->cursor_x = 0;
        vt->cursor_y += n;
        if (vt->cursor_y >= vt->rows) vt->cursor_y = vt->rows - 1;
        break;
    case 'F': /* Cursor prev line */
        vt->cursor_x = 0;
        vt->cursor_y -= n;
        if (vt->cursor_y < 0) vt->cursor_y = 0;
        break;
    case 'G': /* Cursor horizontal absolute */
        vt->cursor_x = (p0 > 0 ? p0 - 1 : 0);
        if (vt->cursor_x >= vt->cols) vt->cursor_x = vt->cols - 1;
        break;
    case 'H': case 'f': /* Cursor position */
        vt->cursor_y = (p0 > 0 ? p0 - 1 : 0);
        vt->cursor_x = (p1 > 0 ? p1 - 1 : 0);
        if (vt->cursor_y >= vt->rows) vt->cursor_y = vt->rows - 1;
        if (vt->cursor_x >= vt->cols) vt->cursor_x = vt->cols - 1;
        break;
    case 'J': /* Erase in display */
        if (p0 == 0) {
            /* Clear from cursor to end */
            for (int x = vt->cursor_x; x < vt->cols; x++)
                set_cell(vt, x, vt->cursor_y, ' ', make_attr(vt));
            for (int y = vt->cursor_y + 1; y < vt->rows; y++)
                for (int x = 0; x < vt->cols; x++)
                    set_cell(vt, x, y, ' ', make_attr(vt));
        } else if (p0 == 1) {
            /* Clear from start to cursor */
            for (int y = 0; y < vt->cursor_y; y++)
                for (int x = 0; x < vt->cols; x++)
                    set_cell(vt, x, y, ' ', make_attr(vt));
            for (int x = 0; x <= vt->cursor_x; x++)
                set_cell(vt, x, vt->cursor_y, ' ', make_attr(vt));
        } else if (p0 == 2 || p0 == 3) {
            /* Clear entire screen */
            for (int y = 0; y < vt->rows; y++)
                for (int x = 0; x < vt->cols; x++)
                    set_cell(vt, x, y, ' ', make_attr(vt));
        }
        break;
    case 'K': /* Erase in line */
        if (p0 == 0) {
            for (int x = vt->cursor_x; x < vt->cols; x++)
                set_cell(vt, x, vt->cursor_y, ' ', make_attr(vt));
        } else if (p0 == 1) {
            for (int x = 0; x <= vt->cursor_x; x++)
                set_cell(vt, x, vt->cursor_y, ' ', make_attr(vt));
        } else if (p0 == 2) {
            for (int x = 0; x < vt->cols; x++)
                set_cell(vt, x, vt->cursor_y, ' ', make_attr(vt));
        }
        break;
    case 'L': { /* Insert lines */
        int at = vt->cursor_y;
        int count = n;
        if (at + count > vt->rows) count = vt->rows - at;
        /* Shift lines down */
        for (int y = vt->rows - 1; y >= at + count; y--) {
            kmemcpy(vt->chars + y * vt->cols, vt->chars + (y - count) * vt->cols,
                    (size_t)vt->cols * sizeof(uint32_t));
            kmemcpy(vt->attrs + y * vt->cols, vt->attrs + (y - count) * vt->cols,
                    (size_t)vt->cols);
        }
        /* Clear inserted lines */
        for (int y = at; y < at + count && y < vt->rows; y++)
            for (int x = 0; x < vt->cols; x++)
                set_cell(vt, x, y, ' ', make_attr(vt));
        break;
    }
    case 'M': { /* Delete lines */
        int at = vt->cursor_y;
        int count = n;
        if (at + count > vt->rows) count = vt->rows - at;
        /* Shift lines up */
        for (int y = at; y < vt->rows - count; y++) {
            kmemcpy(vt->chars + y * vt->cols, vt->chars + (y + count) * vt->cols,
                    (size_t)vt->cols * sizeof(uint32_t));
            kmemcpy(vt->attrs + y * vt->cols, vt->attrs + (y + count) * vt->cols,
                    (size_t)vt->cols);
        }
        /* Clear bottom lines */
        for (int y = vt->rows - count; y < vt->rows; y++)
            for (int x = 0; x < vt->cols; x++)
                set_cell(vt, x, y, ' ', make_attr(vt));
        mark_all_dirty(vt);
        break;
    }
    case 'm': /* SGR */
        if (vt->esc_nparam == 0) {
            apply_sgr(vt, 0);
        } else {
            for (int i = 0; i < vt->esc_nparam; i++)
                apply_sgr(vt, vt->esc_params[i]);
        }
        break;
    case 'h': /* Set mode */
        if (vt->esc_qmark && p0 == 25)
            vt->cursor_visible = 1;
        break;
    case 'l': /* Reset mode */
        if (vt->esc_qmark && p0 == 25)
            vt->cursor_visible = 0;
        break;
    case 'd': /* Cursor vertical absolute */
        vt->cursor_y = (p0 > 0 ? p0 - 1 : 0);
        if (vt->cursor_y >= vt->rows) vt->cursor_y = vt->rows - 1;
        break;
    case '@': { /* Insert characters */
        int count = n;
        if (vt->cursor_x + count > vt->cols) count = vt->cols - vt->cursor_x;
        int y = vt->cursor_y;
        for (int x = vt->cols - 1; x >= vt->cursor_x + count; x--) {
            int dst = cell_idx(vt, x, y);
            int src = cell_idx(vt, x - count, y);
            vt->chars[dst] = vt->chars[src];
            vt->attrs[dst] = vt->attrs[src];
        }
        for (int x = vt->cursor_x; x < vt->cursor_x + count; x++)
            set_cell(vt, x, y, ' ', make_attr(vt));
        break;
    }
    case 'P': { /* Delete characters */
        int count = n;
        int y = vt->cursor_y;
        for (int x = vt->cursor_x; x < vt->cols - count; x++) {
            int dst = cell_idx(vt, x, y);
            int src = cell_idx(vt, x + count, y);
            vt->chars[dst] = vt->chars[src];
            vt->attrs[dst] = vt->attrs[src];
        }
        for (int x = vt->cols - count; x < vt->cols; x++)
            set_cell(vt, x, y, ' ', make_attr(vt));
        break;
    }
    case 'X': { /* Erase characters */
        int count = n;
        for (int i = 0; i < count && vt->cursor_x + i < vt->cols; i++)
            set_cell(vt, vt->cursor_x + i, vt->cursor_y, ' ', make_attr(vt));
        break;
    }
    case 'n': /* Device status report */
        if (p0 == 6) {
            /* Report cursor position → PTY input */
            char resp[24];
            int len = 0;
            resp[len++] = '\033';
            resp[len++] = '[';
            /* row (1-based) */
            int row = vt->cursor_y + 1;
            if (row >= 100) resp[len++] = (char)('0' + row / 100);
            if (row >= 10) resp[len++] = (char)('0' + (row / 10) % 10);
            resp[len++] = (char)('0' + row % 10);
            resp[len++] = ';';
            /* col (1-based) */
            int col = vt->cursor_x + 1;
            if (col >= 100) resp[len++] = (char)('0' + col / 100);
            if (col >= 10) resp[len++] = (char)('0' + (col / 10) % 10);
            resp[len++] = (char)('0' + col % 10);
            resp[len++] = 'R';
            pty_master_write(vt->pty_id, resp, len);
        }
        break;
    default:
        break;
    }
}

/* ── VT putchar (Unicode codepoint → grid) ─────────── */

static void vt_putchar(vt_t *vt, uint32_t cp) {
    switch (cp) {
    case '\n':
        cursor_newline(vt);
        return;
    case '\r':
        vt->cursor_x = 0;
        return;
    case '\t':
        do { cursor_advance(vt); } while (vt->cursor_x % 8 != 0 && vt->cursor_x < vt->cols);
        return;
    case '\b':
        if (vt->cursor_x > 0) vt->cursor_x--;
        return;
    case '\a': /* bell — ignore */
        return;
    default:
        break;
    }

    /* Printable character */
    if (cp < 0x20) return; /* ignore other control chars */
    set_cell(vt, vt->cursor_x, vt->cursor_y, cp, make_attr(vt));
    cursor_advance(vt);
}

/* ── UTF-8 decoder → putchar ───────────────────────── */

void vt_process_byte(int vt_id, uint8_t byte) {
    if (vt_id < 0 || vt_id >= VT_MAX) return;
    vt_t *vt = &vts[vt_id];

    /* ANSI escape state machine */
    if (vt->esc_state == 1) {
        /* Seen ESC */
        if (byte == '[') {
            vt->esc_state = 2;
            vt->esc_nparam = 0;
            vt->esc_qmark = 0;
            for (int i = 0; i < 8; i++) vt->esc_params[i] = 0;
            return;
        }
        if (byte == ']') {
            vt->esc_state = 3; /* OSC — absorb until ST */
            return;
        }
        if (byte == '(') {
            vt->esc_state = 4; /* charset designation — skip next byte */
            return;
        }
        /* ESC c — reset */
        if (byte == 'c') {
            vt->fg_color = 7; vt->bg_color = 0; vt->bold = 0;
            vt->cursor_x = 0; vt->cursor_y = 0;
            vt->cursor_visible = 1;
        }
        vt->esc_state = 0;
        return;
    }
    if (vt->esc_state == 2) {
        /* CSI parameter parsing */
        if (byte == '?') {
            vt->esc_qmark = 1;
            return;
        }
        if (byte >= '0' && byte <= '9') {
            if (vt->esc_nparam == 0) vt->esc_nparam = 1;
            vt->esc_params[vt->esc_nparam - 1] =
                vt->esc_params[vt->esc_nparam - 1] * 10 + (byte - '0');
            return;
        }
        if (byte == ';') {
            if (vt->esc_nparam < 8) vt->esc_nparam++;
            return;
        }
        /* Dispatch CSI command */
        if (byte >= 0x40 && byte <= 0x7E) {
            csi_dispatch(vt, (char)byte);
        }
        vt->esc_state = 0;
        return;
    }
    if (vt->esc_state == 3) {
        /* OSC — absorb until BEL (0x07) or ST (ESC \) */
        if (byte == 0x07 || byte == '\\')
            vt->esc_state = 0;
        return;
    }
    if (vt->esc_state == 4) {
        /* Charset designation — skip one byte */
        vt->esc_state = 0;
        return;
    }

    /* Check for ESC */
    if (byte == 0x1B) {
        vt->esc_state = 1;
        return;
    }

    /* UTF-8 decoder */
    if (vt->utf8_remaining > 0) {
        if ((byte & 0xC0) == 0x80) {
            vt->utf8_codepoint = (vt->utf8_codepoint << 6) | (byte & 0x3F);
            vt->utf8_remaining--;
            if (vt->utf8_remaining == 0)
                vt_putchar(vt, vt->utf8_codepoint);
        } else {
            /* Invalid continuation — reset and process as new byte */
            vt->utf8_remaining = 0;
            /* Fall through to process byte as start of new sequence */
        }
        if (vt->utf8_remaining > 0 || (byte & 0xC0) == 0x80)
            return;
    }

    if (byte < 0x80) {
        vt_putchar(vt, byte);
    } else if ((byte & 0xE0) == 0xC0) {
        vt->utf8_codepoint = byte & 0x1F;
        vt->utf8_remaining = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        vt->utf8_codepoint = byte & 0x0F;
        vt->utf8_remaining = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        vt->utf8_codepoint = byte & 0x07;
        vt->utf8_remaining = 3;
    }
}

/* ── Render dirty lines to framebuffer ─────────────── */

void vt_render_dirty(int vt_id) {
    if (vt_id < 0 || vt_id >= VT_MAX) return;
    if (!fb_available()) return;
    vt_t *vt = &vts[vt_id];
    if (vt->dirty_top >= vt->dirty_bottom) return;

    int gw = fb_glyph_w();
    int gh = fb_glyph_h();

    for (int y = vt->dirty_top; y < vt->dirty_bottom; y++) {
        for (int x = 0; x < vt->cols; x++) {
            int idx = cell_idx(vt, x, y);
            uint32_t cp = vt->chars[idx];
            uint8_t attr = vt->attrs[idx];
            uint8_t fg_idx = attr & 0x0F;
            uint8_t bg_idx = (attr >> 4) & 0x0F;
            uint32_t fg = ansi_colors[fg_idx];
            uint32_t bg = ansi_colors[bg_idx];
            if (cp == 0) cp = ' ';
            int gi = cp_to_glyph(cp);
            fb_blit_glyph(gi, x * gw, y * gh, fg, bg);
        }
    }

    /* Render cursor (inverse block) */
    if (vt->cursor_visible && vt_id == active_vt) {
        int cx = vt->cursor_x;
        int cy = vt->cursor_y;
        if (cx < vt->cols && cy < vt->rows) {
            int idx = cell_idx(vt, cx, cy);
            uint32_t cp = vt->chars[idx];
            uint8_t attr = vt->attrs[idx];
            uint8_t fg_idx = attr & 0x0F;
            uint8_t bg_idx = (attr >> 4) & 0x0F;
            /* Swap fg/bg for cursor */
            uint32_t fg = ansi_colors[bg_idx];
            uint32_t bg_col = ansi_colors[fg_idx];
            if (cp == 0) cp = ' ';
            int gi = cp_to_glyph(cp);
            fb_blit_glyph(gi, cx * gw, cy * gh, fg, bg_col);
        }
    }

    vt->dirty_top = vt->rows;
    vt->dirty_bottom = 0;
}

/* ── Keyboard events ───────────────────────────────── */

void vt_keyboard_event(uint16_t scancode, int pressed) {
    /* Linux KEY_* modifier codes */
    if (scancode == KEY_LEFTSHIFT || scancode == KEY_RIGHTSHIFT) { key_shift = pressed; return; }
    if (scancode == KEY_LEFTCTRL  || scancode == KEY_RIGHTCTRL)  { key_ctrl = pressed; return; }
    if (scancode == KEY_LEFTALT   || scancode == KEY_RIGHTALT)   { key_alt = pressed; return; }

    if (!pressed) return;

    /* Ctrl+Alt+F1-F4 → VT switch */
    if (key_ctrl && key_alt) {
        if (scancode >= KEY_F1 && scancode <= KEY_F4) {
            vt_switch((int)(scancode - KEY_F1));
            return;
        }
    }

    /* Map scancode to character */
    uint8_t ch;
    if (scancode < 256) {
        ch = key_shift ? keymap_shifted[scancode] : keymap_normal[scancode];
    } else {
        return;
    }
    if (ch == 0) return;

    /* Ctrl+key → control character */
    if (key_ctrl && ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch & 0x1F);
    else if (key_ctrl && ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch & 0x1F);

    /* Send to active VT's PTY.
     * Don't flush here — we're in IRQ context.
     * Timer tick or next syscall will flush. */
    char c = (char)ch;
    pty_master_write(vts[active_vt].pty_id, &c, 1);
}

/* ── VT switch ─────────────────────────────────────── */

void vt_switch(int vt_id) {
    if (vt_id < 0 || vt_id >= VT_MAX) return;
    active_vt = vt_id;
    mark_all_dirty(&vts[vt_id]);
    vt_render_dirty(vt_id);
    serial_puts("vt: switch to ");
    serial_putchar('0' + vt_id);
    serial_putchar('\n');
}

/* ── Drain PTY output → VT ─────────────────────────── */

void vt_flush(int vt_id) {
    if (vt_id < 0 || vt_id >= VT_MAX) return;
    vt_t *vt = &vts[vt_id];
    char buf[256];
    int n;
    while ((n = pty_master_read(vt->pty_id, buf, (int)sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++)
            vt_process_byte(vt_id, (uint8_t)buf[i]);
    }
    if (vt_id == active_vt)
        vt_render_dirty(vt_id);
}

/* ── Query ─────────────────────────────────────────── */

int vt_cols(void) { return grid_cols; }
int vt_rows(void) { return grid_rows; }
int vt_pty_id(int vt_id) {
    if (vt_id < 0 || vt_id >= VT_MAX) return -1;
    return vts[vt_id].pty_id;
}

/* ── Helper to print number to serial ──────────────── */

static void serial_int(int v) {
    if (v == 0) { serial_putchar('0'); return; }
    char t[10]; int j = 0;
    while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
    while (j--) serial_putchar(t[j]);
}

/* ── Init ──────────────────────────────────────────── */

void vt_init(struct boot_info *info) {
    /* Initialize framebuffer */
    fb_init(info);

    /* Initialize PTYs */
    pty_init();

    /* Compute grid size */
    if (fb_available()) {
        grid_cols = fb_width() / fb_glyph_w();
        grid_rows = fb_height() / fb_glyph_h();
    } else {
        grid_cols = 80;
        grid_rows = 24;
    }

    /* Allocate and init VTs */
    for (int i = 0; i < VT_MAX; i++) {
        vt_t *vt = &vts[i];
        vt->cols = grid_cols;
        vt->rows = grid_rows;

        /* Allocate character + attribute buffers */
        int cells = grid_cols * grid_rows;
        int char_pages = (cells * (int)sizeof(uint32_t) + 4095) / 4096;
        int attr_pages = (cells + 4095) / 4096;
        vt->chars = (uint32_t *)pages_alloc(char_pages);
        vt->attrs = (uint8_t *)pages_alloc(attr_pages);

        if (!vt->chars || !vt->attrs) {
            serial_puts("vt: alloc failed for VT ");
            serial_putchar('0' + i);
            serial_putchar('\n');
            continue;
        }
        kmemset(vt->chars, 0, (size_t)cells * sizeof(uint32_t));
        kmemset(vt->attrs, 0, (size_t)cells);

        vt->cursor_x = 0;
        vt->cursor_y = 0;
        vt->cursor_visible = 1;
        vt->esc_state = 0;
        vt->fg_color = 7;  /* white */
        vt->bg_color = 0;  /* black */
        vt->bold = 0;
        vt->utf8_remaining = 0;
        vt->dirty_top = grid_rows;
        vt->dirty_bottom = 0;

        /* Allocate PTY */
        vt->pty_id = pty_alloc();
    }

    active_vt = 0;

    serial_puts("vt: ");
    serial_putchar('0' + VT_MAX);
    serial_puts(" terminals, ");
    serial_int(grid_cols);
    serial_putchar('x');
    serial_int(grid_rows);
    serial_puts(" chars\n");
}
