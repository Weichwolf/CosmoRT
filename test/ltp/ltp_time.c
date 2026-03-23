/* LTP: clock_gettime/gettimeofday/nanosleep/clock_nanosleep */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

int main(void) {
    printf("=== LTP: time ===\n");

    /* 1. clock_gettime CLOCK_REALTIME */
    struct timespec ts;
    int r = clock_gettime(CLOCK_REALTIME, &ts);
    CHECK("clock_gettime REALTIME", r == 0);
    CHECK("REALTIME plausible (>2020)", ts.tv_sec > 1577836800L);

    /* 2. clock_gettime CLOCK_MONOTONIC */
    r = clock_gettime(CLOCK_MONOTONIC, &ts);
    CHECK("clock_gettime MONOTONIC", r == 0 && ts.tv_sec >= 0);

    /* 3. MONOTONIC is monotonic */
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* busy wait a tiny bit */
    for (volatile int i = 0; i < 100000; i++);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long d = (t2.tv_sec - t1.tv_sec) * 1000000000L + (t2.tv_nsec - t1.tv_nsec);
    CHECK("MONOTONIC increases", d > 0);

    /* 4. gettimeofday */
    struct timeval tv;
    r = gettimeofday(&tv, NULL);
    CHECK("gettimeofday", r == 0 && tv.tv_sec > 1577836800L);

    /* 5. gettimeofday ~ clock_gettime REALTIME */
    clock_gettime(CLOCK_REALTIME, &ts);
    gettimeofday(&tv, NULL);
    long diff = (tv.tv_sec - ts.tv_sec) * 1000000L
                + tv.tv_usec - ts.tv_nsec / 1000;
    CHECK("gettimeofday ≈ REALTIME (within 100ms)",
          diff > -100000 && diff < 100000);

    /* 6. nanosleep */
    struct timespec req = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10ms */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r = nanosleep(&req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    d = (t2.tv_sec - t1.tv_sec) * 1000000000L + (t2.tv_nsec - t1.tv_nsec);
    CHECK("nanosleep 10ms", r == 0 && d >= 9000000); /* at least 9ms */

    /* 7. nanosleep 0 returns immediately */
    struct timespec zero = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &t1);
    nanosleep(&zero, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    d = (t2.tv_sec - t1.tv_sec) * 1000000000L + (t2.tv_nsec - t1.tv_nsec);
    CHECK("nanosleep 0 fast (<1ms)", d < 1000000);

    /* 8. clock_getres MONOTONIC */
    struct timespec res;
    r = clock_getres(CLOCK_MONOTONIC, &res);
    CHECK("clock_getres MONOTONIC", r == 0);
    CHECK("resolution <= 1ms", res.tv_sec == 0 && res.tv_nsec <= 1000000);

    /* 9. clock_gettime bad clock → EINVAL */
    errno = 0;
    r = clock_gettime(9999, &ts);
    CHECK_ERRNO("clock_gettime bad clock → EINVAL", r == -1, EINVAL);

    /* 10. time() returns seconds */
    time_t now = time(NULL);
    CHECK("time() plausible", now > 1577836800L);

    /* 11. clock_nanosleep CLOCK_MONOTONIC */
    req = (struct timespec){ .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    d = (t2.tv_sec - t1.tv_sec) * 1000000000L + (t2.tv_nsec - t1.tv_nsec);
    CHECK("clock_nanosleep 5ms", r == 0 && d >= 4000000);

    /* 12. CLOCK_MONOTONIC_RAW (if available) */
    #ifdef CLOCK_MONOTONIC_RAW
    r = clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    CHECK("clock_gettime MONOTONIC_RAW", r == 0);
    #else
    printf("  SKIP  CLOCK_MONOTONIC_RAW not defined\n");
    _ltp_pass++;
    #endif

    LTP_SUMMARY("time");
}
