/* LTP process_vm_readv/process_vm_writev — ported to ktest */
#include "ktest.h"

/* CosmoRT has no process_vm_readv/writev support. Returns -ENOSYS. */

struct k_iovec {
    void  *iov_base;
    size_t iov_len;
};

static void test_process_vm_readv_enosys(void) {
    puts("\n[ltp/process_vm_readv]\n");

    char buf[16];
    struct k_iovec local  = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct k_iovec remote = { .iov_base = buf, .iov_len = sizeof(buf) };

    long pid = sc0(SYS_GETPID);
    long r = sc6(SYS_PROCESS_VM_READV, pid,
                 (long)&local, 1, (long)&remote, 1, 0);
    check_val("process_vm_readv ENOSYS", r, -ENOSYS);
}

static void test_process_vm_writev_enosys(void) {
    puts("\n[ltp/process_vm_writev]\n");

    char buf[16] = "test data";
    struct k_iovec local  = { .iov_base = buf, .iov_len = 9 };
    struct k_iovec remote = { .iov_base = buf, .iov_len = 9 };

    long pid = sc0(SYS_GETPID);
    long r = sc6(SYS_PROCESS_VM_WRITEV, pid,
                 (long)&local, 1, (long)&remote, 1, 0);
    check_val("process_vm_writev ENOSYS", r, -ENOSYS);
}

TEST("ltp/process_vm_readv",  test_process_vm_readv_enosys);
TEST("ltp/process_vm_writev", test_process_vm_writev_enosys);
