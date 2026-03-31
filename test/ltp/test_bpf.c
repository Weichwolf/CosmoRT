/* LTP bpf_map01/bpf_prog01-04 — ported to ktest */
#include "ktest.h"

/* CosmoRT has no BPF support. bpf() returns -ENOSYS. */

#define BPF_MAP_CREATE  0
#define BPF_PROG_LOAD   5

struct bpf_attr_small {
    uint32_t map_type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
};

static void test_bpf_map_enosys(void) {
    puts("\n[ltp/bpf_map01]\n");

    struct bpf_attr_small attr = {
        .map_type    = 1, /* BPF_MAP_TYPE_HASH */
        .key_size    = 4,
        .value_size  = 4,
        .max_entries = 1,
    };

    long r = sc3(SYS_BPF, BPF_MAP_CREATE, (long)&attr, (long)sizeof(attr));
    check_val("bpf MAP_CREATE ENOSYS", r, -ENOSYS);
}

static void test_bpf_prog_enosys(void) {
    puts("\n[ltp/bpf_prog01]\n");

    /* minimal attr, content irrelevant — syscall should fail immediately */
    char attr[128];
    for (int i = 0; i < 128; i++) attr[i] = 0;

    long r = sc3(SYS_BPF, BPF_PROG_LOAD, (long)attr, 128);
    check_val("bpf PROG_LOAD ENOSYS", r, -ENOSYS);
}

TEST("ltp/bpf_map01",  test_bpf_map_enosys);
TEST("ltp/bpf_prog01", test_bpf_prog_enosys);
