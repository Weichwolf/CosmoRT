/* CosmoRT Hardware Test — parallel test runner
 * ALL tests (unit + crash) run in fork'd children.
 * Batch of BATCH children run concurrently.
 * Parent collects results via MAP_SHARED slots + wait4.
 * Tests self-register via .ktest linker section. */
#include "ktest.h"

int failures = 0;
int passes = 0;

extern const ktest_entry_t __start_ktest[];
extern const ktest_entry_t __stop_ktest[];

#define BATCH 1

/* Allocate MAP_SHARED anonymous memory */
static void *shared_alloc(long size) {
    return (void *)sc6(SYS_MMAP, 0, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}


void _start_c(void) {
    passes = 0;
    failures = 0;
    puts("\n=== CosmoRT Hardware Test ===\n");

    int total = (int)(__stop_ktest - __start_ktest);

    /* Allocate shared slots for result reporting */
    test_slot_t *slots = (test_slot_t *)shared_alloc(BATCH * (long)sizeof(test_slot_t));
    if ((long)slots < 0) {
        puts("FATAL: shared_alloc failed\n");
        sc1(SYS_EXIT_GROUP, 1);
        __builtin_unreachable();
    }

    int idx = 0;
    while (idx < total) {
        int batch_size = total - idx;
        if (batch_size > BATCH) batch_size = BATCH;

        /* Clear slots */
        for (int s = 0; s < batch_size; s++) {
            slots[s].done = 0;
            slots[s].pass_cnt = 0;
            slots[s].fail_cnt = 0;
            slots[s].exit_code = -1;
            slots[s].pid = 0;
        }

        /* Fork children */
        for (int s = 0; s < batch_size; s++) {
            const ktest_entry_t *t = &__start_ktest[idx + s];
            long pid = sc0(SYS_FORK);
            if (pid < 0) {
                puts("  FAIL  "); puts(t->name); puts(" (fork failed)\n");
                slots[s].done = 1;
                slots[s].fail_cnt = 1;
                failures++;
                continue;
            }
            if (pid == 0) {
                /* Child: own process group + session so tests that check
                 * getpgrp()==getpid() work correctly in forked context. */
                sc2(SYS_SETPGID, 0, 0);
                sc0(SYS_SETSID);
                failures = 0;
                passes = 0;
                t->fn();
                slots[s].pass_cnt = passes;
                slots[s].fail_cnt = failures;
                slots[s].done = 1;
                sc1(SYS_EXIT_GROUP, (long)failures);
                __builtin_unreachable();
            }
            slots[s].pid = pid;
        }

        /* Wait for all children in this batch.
         * Timeout: 1000 yield iterations per child. */
        for (int s = 0; s < batch_size; s++) {
            if (slots[s].pid <= 0) continue;

            int status = 0;
            long w = sc4(SYS_WAIT4, slots[s].pid, (long)&status, 0, 0);

            const ktest_entry_t *t = &__start_ktest[idx + s];

            if (w < 0) {
                puts("  FAIL  "); puts(t->name); puts(" (wait4 failed)\n");
                failures++;
                continue;
            }

            /* Decode wait status */
            int exit_signal = status & 0x7F;
            int exit_code = exit_signal ? (128 + exit_signal) : ((status >> 8) & 0xFF);
            slots[s].exit_code = exit_code;

            /* Collect results from shared slot */
            if (!slots[s].done) {
                /* Child died before writing results */
                puts("  --- "); puts(t->name); puts(": CRASHED (status=");
                put_int(exit_code); puts(")\n");
                failures++;
            } else if (slots[s].fail_cnt == 0 && exit_code == 0) {
                passes += slots[s].pass_cnt;
                if (t->crash) {
                    puts("  --- "); puts(t->name); puts(": child OK ---\n");
                }
            } else {
                passes += slots[s].pass_cnt;
                failures += slots[s].fail_cnt;
                if (slots[s].fail_cnt == 0 && exit_code != 0) {
                    puts("  FAIL  "); puts(t->name); puts(" (exit=");
                    put_int(exit_code); puts(")\n");
                    failures++;
                }
            }
        }

        idx += batch_size;
    }

    /* Unmap shared slots */
    sc2(SYS_MUNMAP, (long)slots, BATCH * (long)sizeof(test_slot_t));

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");
    if (failures == 0) puts("ALL PASSED\n");

    /* Power off */
    sc3(SYS_REBOOT, (long)0xFEE1DEAD, (long)0x28121969, (long)0x4321FEDC);
    sc1(SYS_EXIT_GROUP, (long)failures);
    __builtin_unreachable();
}
