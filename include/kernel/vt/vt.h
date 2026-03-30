/* CosmoRT Virtual Terminal — character grid + ANSI parser + FB renderer */
#ifndef VT_H
#define VT_H

#include <stdint.h>

struct boot_info;

void vt_init(struct boot_info *info);

void vt_process_byte(int vt_id, uint8_t byte);

void vt_switch(int vt_id);

void vt_keyboard_event(uint16_t scancode, int pressed);

void vt_render_dirty(int vt_id);

int vt_cols(void);
int vt_rows(void);

int vt_pty_id(int vt_id);

void vt_flush(int vt_id);

#endif
