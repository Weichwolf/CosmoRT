/* LTP mini-framework — shared by all ltp_*.c tests */
#ifndef LTP_H
#define LTP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int _ltp_pass, _ltp_fail;

#define CHECK(name, cond) do { \
    if (cond) { printf("  PASS  %s\n", name); _ltp_pass++; } \
    else { printf("  FAIL  %s (errno=%d %s)\n", name, errno, strerror(errno)); _ltp_fail++; } \
} while(0)

#define CHECK_ERRNO(name, cond, expected_errno) do { \
    if ((cond) && errno == (expected_errno)) { printf("  PASS  %s\n", name); _ltp_pass++; } \
    else { printf("  FAIL  %s (errno=%d %s, expected %d)\n", name, errno, strerror(errno), expected_errno); _ltp_fail++; } \
} while(0)

#define LTP_SUMMARY(suite) do { \
    printf("=== %s: %d passed, %d failed ===\n", suite, _ltp_pass, _ltp_fail); \
    return _ltp_fail > 0 ? 1 : 0; \
} while(0)

#endif
