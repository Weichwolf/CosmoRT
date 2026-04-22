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
#define USEC_PER_SEC            1000000L
#define NSEC_PER_USEC           1000L

/* adjtimex / clock_adjtime — Linux ABI (sys/timex.h) */
#define ADJ_OFFSET              0x0001
#define ADJ_FREQUENCY           0x0002
#define ADJ_MAXERROR            0x0004
#define ADJ_ESTERROR            0x0008
#define ADJ_STATUS              0x0010
#define ADJ_TIMECONST           0x0020
#define ADJ_TAI                 0x0080
#define ADJ_SETOFFSET           0x0100
#define ADJ_MICRO               0x1000
#define ADJ_NANO                0x2000
#define ADJ_TICK                0x4000
#define ADJ_OFFSET_SINGLESHOT   0x8001
#define ADJ_OFFSET_SS_READ      0xa001
#define ADJ_ALL                 (ADJ_OFFSET | ADJ_FREQUENCY | ADJ_MAXERROR |    \
                                 ADJ_ESTERROR | ADJ_STATUS | ADJ_TIMECONST |    \
                                 ADJ_TICK)

/* STA_* — clock status/command bits (timex.status) */
#define STA_PLL                 0x0001
#define STA_PPSFREQ             0x0002
#define STA_PPSTIME             0x0004
#define STA_FLL                 0x0008
#define STA_INS                 0x0010
#define STA_DEL                 0x0020
#define STA_UNSYNC              0x0040
#define STA_FREQHOLD            0x0080
#define STA_PPSSIGNAL           0x0100
#define STA_PPSJITTER           0x0200
#define STA_PPSWANDER           0x0400
#define STA_PPSERROR            0x0800
#define STA_CLOCKERR            0x1000
#define STA_NANO                0x2000
#define STA_MODE                0x4000
#define STA_CLK                 0x8000
#define STA_RONLY               (STA_PPSSIGNAL | STA_PPSJITTER | STA_PPSWANDER | \
                                 STA_PPSERROR | STA_CLOCKERR | STA_NANO |        \
                                 STA_MODE | STA_CLK)

/* Clock-command return values (adjtimex/clock_adjtime) */
#define TIME_OK                 0
#define TIME_INS                1
#define TIME_DEL                2
#define TIME_OOP                3
#define TIME_WAIT               4
#define TIME_ERROR              5
#define TIME_BAD                TIME_ERROR

/* Tick limits (HZ=100 → 1e6/HZ µs nominal = 10000, tolerance ±10%) */
#define TIMEX_TICK_NOMINAL      10000L
#define TIMEX_TICK_MIN          9000L
#define TIMEX_TICK_MAX          11000L

/* Offset limits */
#define TIMEX_MAXPHASE          500000000L
#define TIMEX_MAXFREQ           32768000L

#endif /* COSMO_LINUX_TIME_H */
