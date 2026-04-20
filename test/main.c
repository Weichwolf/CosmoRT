/* CosmoRT Hardware Test — sequential runner with parent-side watchdog.
 *
 * Parent forkt, installiert SIGALRM-Handler, setzt alarm(timeout), blocking wait4.
 * Handler setzt Flag + kill(-pgid, SIGKILL). wait4 returned mit -EINTR (vom Handler)
 * oder mit child-pid (vom SIGKILL-induced exit). Zweites wait4 reapt bei Bedarf.
 *
 * Kein alarm() im Child — kollidiert sonst mit Tests die selbst alarm() benutzen
 * (LTP alarm02..07). Kein poll-loop mit nanosleep — nanosleep hangt nach bestimmten
 * Test-Sequenzen (SIGSTOP/SIGCONT/SIGKILL auf Grandchild); blocking wait4 umgeht
 * den Pfad. */
#include "ktest.h"

#define TIMEOUT_DEFAULT_MS  5000
#define TIMEOUT_NET_MS     10000
#define TIMEOUT_CRASH_MS   15000
#define TIMEOUT_FUZZ_MS    15000

#define ALARM_SLACK_MS       100

int failures = 0;
int passes = 0;

extern const ktest_entry_t __start_ktest[];
extern const ktest_entry_t __stop_ktest[];

static volatile int alarm_fired;
static volatile long watchdog_pid;

__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}

static void sigalrm_watchdog(int sig) {
    (void)sig;
    alarm_fired = 1;
    long vp = watchdog_pid;
    if (vp > 0) sc2(SYS_KILL, -vp, SIGKILL);
}

static void install_watchdog_handler(void) {
    struct k_sigaction sa = { 0 };
    sa.sa_handler = (void *)sigalrm_watchdog;
    sa.sa_flags = SA_RESTORER;
    sa.sa_restorer = (void *)sig_restorer;
    sa.sa_mask = 0;
    sc4(SYS_RT_SIGACTION, SIGALRM, (long)&sa, 0, 8);
}

static void *shared_alloc(long size) {
    return (void *)sc6(SYS_MMAP, 0, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

static int str_has_prefix(const char *s, const char *pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

static long timeout_seconds_for(const char *name) {
    long ms;
    if      (str_has_prefix(name, "net/"))   ms = TIMEOUT_NET_MS;
    else if (str_has_prefix(name, "crash/")) ms = TIMEOUT_CRASH_MS;
    else if (str_has_prefix(name, "fuzz/"))  ms = TIMEOUT_FUZZ_MS;
    else                                      ms = TIMEOUT_DEFAULT_MS;
    return (ms + ALARM_SLACK_MS + 999) / 1000;
}

void _start_c(void) {
    passes = 0;
    failures = 0;
    puts("\n=== CosmoRT Hardware Test ===\n");

    install_watchdog_handler();

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

        long pid = sc0(SYS_FORK);
        if (pid < 0) {
            puts("  FAIL  "); puts(t->name); puts(" (fork failed)\n");
            failures++;
            continue;
        }
        if (pid == 0) {
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
        watchdog_pid = pid;
        alarm_fired = 0;
        sc1(SYS_ALARM, timeout_seconds_for(t->name));

        int status = 0;
        long w;
        do {
            w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        } while (w == -EINTR);

        sc1(SYS_ALARM, 0);
        watchdog_pid = 0;

        int timed_out = alarm_fired;

        if (w < 0) {
            puts("  FAIL  "); puts(t->name); puts(" (wait4 failed)\n");
            failures++;
            continue;
        }

        int exit_signal = status & 0x7F;
        int exit_code = exit_signal ? (128 + exit_signal) : ((status >> 8) & 0xFF);
        slot->exit_code = exit_code;

        if (timed_out) {
            puts("  FAIL  "); puts(t->name); puts(" (TIMEOUT)\n");
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
