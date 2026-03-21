/* CosmoRT Input Subsystem — driver-agnostic keyboard/mouse routing
 *
 * Analogous to net.h: drivers register via input_register(),
 * submit events via input_submit_event(). Subsystem routes to VT.
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* Input event (matches Linux input_event layout) */
typedef struct {
    uint16_t type;   /* EV_KEY=1, EV_REL=2, EV_ABS=3 */
    uint16_t code;   /* KEY_A, REL_X, etc. */
    int32_t  value;  /* 1=press, 0=release, axis value */
} input_event_t;

/* Input driver interface — any keyboard/mouse driver implements this */
typedef struct {
    const char *name;
    /* Driver calls input_submit_event, no poll needed */
} input_driver_t;

void input_init(void);
void input_register(const input_driver_t *drv);
int  input_has_keyboard(void);

/* Called BY drivers when they receive an event */
void input_submit_event(const input_event_t *ev);

#endif
