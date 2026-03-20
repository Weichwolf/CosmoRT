/* SMP — Start Application Processor (Core 1) */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "config.h"

/* Start all APs (1..N-1). entry_fn called on each AP with own stack. */
int smp_start_all(void (*entry_fn)(void));

/* Number of running cores (including BSP) */
int smp_num_cores(void);

/* Check if specific core is running */
int smp_core_running(int core);

/* Get current core APIC ID */
int smp_core_id(void);

#endif
