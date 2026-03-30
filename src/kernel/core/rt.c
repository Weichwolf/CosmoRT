/* CosmoRT RT-Core identification */

#include "core/rt.h"
#include "core/smp.h"

int rt_core_id(int index) {
    if (index < 0 || index >= RT_CORE_COUNT) return -1;
    return 0;
}

int rt_is_current_rt(void) {
    return smp_core_id() == 0;
}
