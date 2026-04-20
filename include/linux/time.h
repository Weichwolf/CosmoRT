/* Linux x86_64 ABI — clock IDs */
#ifndef COSMO_LINUX_TIME_H
#define COSMO_LINUX_TIME_H

#include "types.h"

#define CLOCK_REALTIME          0
#define CLOCK_MONOTONIC         1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW     4
#define CLOCK_REALTIME_COARSE   5
#define CLOCK_MONOTONIC_COARSE  6
#define CLOCK_BOOTTIME          7
#define TIMER_ABSTIME           1

#define NSEC_PER_SEC            1000000000L
#define MSEC_PER_SEC            1000L
#define NSEC_PER_MSEC           1000000L

#endif /* COSMO_LINUX_TIME_H */
