/* SMP — Start Application Processor (Core 1) */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "config.h"

int smp_start_all(void (*entry_fn)(void));

int smp_num_cores(void);

int smp_core_running(int core);

int smp_core_id(void);

void rt_wake(int core_id);

#endif
