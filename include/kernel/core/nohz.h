/* CosmoRT NOHZ_FULL — tickless operation on isolated cores */
#ifndef NOHZ_H
#define NOHZ_H

#include <stdint.h>

void nohz_enter_tickless(int cpu);
void nohz_exit_tickless(int cpu);
int  nohz_is_tickless(int cpu);
void nohz_arm_oneshot(int cpu, uint64_t deadline_tsc);
void nohz_cancel_oneshot(int cpu);

#endif
