/* CosmoRT Virtual Terminal — character grid + ANSI parser + FB renderer
 *
 * 4 VTs, each with PTY, character buffer, ANSI escape parser.
 * Renders to framebuffer via fb.c glyph blitter.
 * Keyboard events routed to active VT's PTY master.
 */
#ifndef VT_H
#define VT_H

#include <stdint.h>

struct boot_info;

/* Initialize VT subsystem (FB, PTY, 4 terminals). */
void vt_init(struct boot_info *info);

/* Process one byte from a process's output (UTF-8 → ANSI parser → char grid). */
void vt_process_byte(int vt_id, uint8_t byte);

/* Switch active VT (0..VT_MAX-1). */
void vt_switch(int vt_id);

/* Keyboard event from input driver (USB HID scancode, pressed=1/released=0). */
void vt_keyboard_event(uint16_t scancode, int pressed);

/* Keyboard event from Hyper-V (AT make_code, key_up flag). */
void vt_keyboard_event_at(uint16_t make_code, int key_up);

/* Render dirty lines of a VT to the framebuffer. */
void vt_render_dirty(int vt_id);

/* Get VT grid dimensions (for ioctl TIOCGWINSZ). */
int vt_cols(void);
int vt_rows(void);

/* Get PTY id for a VT. */
int vt_pty_id(int vt_id);

/* Drain PTY output and render (called from timer tick or explicit flush). */
void vt_flush(int vt_id);

/* Keyboard presence (for deciding whether init gets PTY or serial fds). */
int vt_has_keyboard(void);
void vt_set_keyboard(void);

#endif
