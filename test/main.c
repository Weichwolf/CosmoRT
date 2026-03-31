/* CosmoRT Hardware Test — sequential runner with per-test timeout
 * ALL tests run in fork'd children, one at a time.
 * Timeout: parent forks a watchdog that kills the test child after
 * TEST_TIMEOUT seconds. Tests cannot interfere with this mechanism.
 * Tests self-register via .ktest linker section. */
#include "ktest.h"

#define SIGKILL  9
#define SIGALRM 14
#define TEST_TIMEOUT 5

int failures = 0;
int passes = 0;

extern const ktest_entry_t __start_ktest[];
extern const ktest_entry_t __stop_ktest[];

/* Allocate MAP_SHARED anonymous memory */
static void *shared_alloc(long size) {
    return (void *)sc6(SYS_MMAP, 0, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

static void sleep_s(int sec) {
    struct k_timespec ts = { .tv_sec = sec, .tv_nsec = 0 };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

void _start_c(void) {
    passes = 0;
    failures = 0;
    puts("\n=== CosmoRT Hardware Test ===\n");

    int total = (int)(__stop_ktest - __start_ktest);

    test_slot_t *slot = (test_slot_t *)shared_alloc((long)sizeof(test_slot_t));
    if ((long)slot < 0) {
        puts("FATAL: shared_alloc failed\n");
        sc1(SYS_EXIT_GROUP, 1);
        __builtin_unreachable();
    }

    for (int idx = 0; idx < total; idx++) {
        const ktest_entry_t *t = &__start_ktest[idx];

        slot->done = 0;
        slot->pass_cnt = 0;
        slot->fail_cnt = 0;
        slot->exit_code = -1;
        slot->pid = 0;

        /* Fork test child */
        long pid = sc0(SYS_FORK);
        if (pid < 0) {
            puts("  FAIL  "); puts(t->name); puts(" (fork failed)\n");
            failures++;
            continue;
        }
        if (pid == 0) {
            /* Test child: clean slate, no runner alarm */
            sc2(SYS_SETPGID, 0, 0);
            sc0(SYS_SETSID);
            failures = 0;
            passes = 0;
            t->fn();
            slot->pass_cnt = passes;
            slot->fail_cnt = failures;
            slot->done = 1;
            sc1(SYS_EXIT_GROUP, (long)failures);
            __builtin_unreachable();
        }

        slot->pid = pid;

        /* Fork watchdog: sleeps TEST_TIMEOUT, then SIGKILL the test child.
         * Separate process — test child cannot interfere. */
        long wdog = sc0(SYS_FORK);
        if (wdog == 0) {
            sleep_s(TEST_TIMEOUT);
            sc2(SYS_KILL, pid, SIGKILL);
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }

        /* Parent: blocking wait for test child */
        int status = 0;
        long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);

        /* Test child done — kill watchdog */
        if (wdog > 0) {
            sc2(SYS_KILL, wdog, SIGKILL);
            sc4(SYS_WAIT4, wdog, 0, 0, 0);
        }

        if (w < 0) {
            puts("  FAIL  "); puts(t->name); puts(" (wait4 failed)\n");
            failures++;
            continue;
        }

        int exit_signal = status & 0x7F;
        int exit_code = exit_signal ? (128 + exit_signal) : ((status >> 8) & 0xFF);
        slot->exit_code = exit_code;

        if (exit_signal == SIGKILL) {
            /* SIGKILL from watchdog = timeout */
            puts("  FAIL  "); puts(t->name); puts(" (TIMEOUT)\n");
            failures++;
        } else if (exit_signal == SIGALRM) {
            /* Test's own alarm killed it */
            puts("  FAIL  "); puts(t->name); puts(" (SIGALRM)\n");
            failures++;
        } else if (!slot->done) {
            puts("  --- "); puts(t->name); puts(": CRASHED (status=");
            put_int(exit_code); puts(")\n");
            failures++;
        } else if (slot->fail_cnt == 0 && exit_code == 0) {
            passes += slot->pass_cnt;
            if (t->crash) {
                puts("  --- "); puts(t->name); puts(": child OK ---\n");
            }
        } else {
            passes += slot->pass_cnt;
            failures += slot->fail_cnt;
            if (slot->fail_cnt == 0 && exit_code != 0) {
                puts("  FAIL  "); puts(t->name); puts(" (exit=");
                put_int(exit_code); puts(")\n");
                failures++;
            }
        }
    }

    sc2(SYS_MUNMAP, (long)slot, (long)sizeof(test_slot_t));

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");
    if (failures == 0) puts("ALL PASSED\n");

    sc3(SYS_REBOOT, (long)0xFEE1DEAD, (long)0x28121969, (long)0x4321FEDC);
    sc1(SYS_EXIT_GROUP, (long)failures);
    __builtin_unreachable();
}
