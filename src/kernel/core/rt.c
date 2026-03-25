/* CosmoRT RT-Core identification
 *
 * RT-Core 0 = physical Core 0 (BSP). All IRQs routed here.
 * Compute-Cores = Core 1..N, run userspace exclusively.
 */

#include "rt.h"
#include "smp.h"

int rt_core_id(int index) {
    if (index < 0 || index >= RT_CORE_COUNT) return -1;
    return 0;  /* RT-Core 0 = physical Core 0 */
}

int rt_is_current_rt(void) {
    return smp_core_id() == 0;
}
