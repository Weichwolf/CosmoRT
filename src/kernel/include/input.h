/* CosmoRT Input Subsystem — driver-agnostic keyboard/mouse routing
 *
 * Analogous to net.h: drivers register via input_register(),
 * submit events via input_submit_event(). Subsystem routes to VT.
 *
 * input_event_t, input_driver_t, input_register, input_submit_event
 * are declared in cosmo_hw.h (public driver API).
 */
#ifndef INPUT_H
#define INPUT_H

#include "cosmo_hw.h"

void input_init(void);
int  input_has_keyboard(void);

#endif
