/* CosmoRT Hardware Test — entry point */
#include "ktest.h"

int failures = 0;
int passes = 0;

extern void test_identity(void);
extern void test_memory(void);
extern void test_tls(void);
extern void test_time(void);
extern void test_random(void);
extern void test_vfs(void);
extern void test_procfs(void);
extern void test_threads(void);
extern void test_pci(void);
extern void test_security(void);
extern void test_yield(void);
extern void test_signals(void);
extern void test_vm_patterns(void);

void _start(void) {
    puts("\n=== CosmoRT Hardware Test ===\n");

    test_identity();
    test_memory();
    test_tls();
    test_time();
    test_random();
    test_vfs();
    test_procfs();
    test_threads();
    test_pci();
    test_security();
    test_yield();
    test_signals();
    test_vm_patterns();

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");

    if (failures == 0)
        puts("ALL PASSED\n");

    sc1(SYS_exit_group, (long)failures);
    __builtin_unreachable();
}
