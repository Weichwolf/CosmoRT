/* SMP — single-core stub */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

int smp_num_cores(void);
int smp_core_running(int core);
int smp_core_id(void);

#endif
