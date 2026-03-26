#include "ktest.h"

#define TEST_PATH "/tmp/shmmap_test"
#define PAGE 4096

/* Helper: create file, write pattern, return fd (caller closes) */
static long create_test_file(const char *path, uint64_t pattern) {
    long fd = sc3(SYS_OPEN, (long)path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return fd;
    /* Write one page of repeated pattern */
    uint64_t buf[PAGE / 8];
    for (int i = 0; i < PAGE / 8; i++) buf[i] = pattern;
    sc3(SYS_WRITE, fd, (long)buf, PAGE);
    return fd;
}

/* test_shared_mmap_basic: mmap MAP_SHARED, verify data readable */
static void test_shared_mmap_basic(void) {
    puts("\n[shared_mmap basic]\n");

    long fd = create_test_file(TEST_PATH, 0xABCD1234ABCD1234ULL);
    check("create file", fd >= 0);

    long addr = sc6(SYS_MMAP, 0, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    check("mmap shared", addr > 0x1000);

    volatile uint64_t *p = (volatile uint64_t *)addr;
    check_val("data correct", (long)*p, (long)0xABCD1234ABCD1234ULL);

    sc2(SYS_MUNMAP, addr, PAGE);
    sc1(SYS_CLOSE, fd);
}

/* test_shared_mmap_fork: MAP_SHARED + fork, both see same data */
static void test_shared_mmap_fork(void) {
    puts("\n[shared_mmap fork]\n");

    long fd = create_test_file(TEST_PATH, 0xDEADFACEDEADFACEULL);
    check("create file", fd >= 0);

    long addr = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap shared", addr > 0x1000);

    volatile uint64_t *p = (volatile uint64_t *)addr;
    check_val("parent sees data", (long)*p, (long)0xDEADFACEDEADFACEULL);

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);

    if (pid == 0) {
        /* Child: verify same data visible */
        if (*p != 0xDEADFACEDEADFACEULL)
            sc1(SYS_EXIT_GROUP, 1);
        sc1(SYS_EXIT_GROUP, 0);
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited 0", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);

    sc2(SYS_MUNMAP, addr, PAGE);
    sc1(SYS_CLOSE, fd);
}

/* test_shared_mmap_two_maps: mmap same file twice, both see same data */
static void test_shared_mmap_two_maps(void) {
    puts("\n[shared_mmap two maps]\n");

    long fd = create_test_file(TEST_PATH, 0xCAFEBABECAFEBABEULL);
    check("create file", fd >= 0);

    long addr1 = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap 1", addr1 > 0x1000);

    long addr2 = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap 2", addr2 > 0x1000);

    volatile uint64_t *p1 = (volatile uint64_t *)addr1;
    volatile uint64_t *p2 = (volatile uint64_t *)addr2;

    check_val("map1 data", (long)*p1, (long)0xCAFEBABECAFEBABEULL);
    check_val("map2 data", (long)*p2, (long)0xCAFEBABECAFEBABEULL);

    /* Write through map1, visible in map2 (same physical page) */
    *p1 = 0x1111222233334444ULL;
    check_val("map2 sees write", (long)*p2, (long)0x1111222233334444ULL);

    sc2(SYS_MUNMAP, addr1, PAGE);
    sc2(SYS_MUNMAP, addr2, PAGE);
    sc1(SYS_CLOSE, fd);
}

TEST("shared_mmap_basic", test_shared_mmap_basic);
TEST("shared_mmap_fork", test_shared_mmap_fork);
TEST("shared_mmap_two_maps", test_shared_mmap_two_maps);
