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

/* test_demand_paging_file: mmap file, verify fault handler reads correct data */
static void test_demand_paging_file(void) {
    puts("\n[demand_paging file]\n");

    /* Create a file with known pattern */
    long fd = create_test_file("/tmp/dptest", 0xFEEDC0DEFEEDC0DEULL);
    check("create file", fd >= 0);

    /* Close and reopen to ensure page cache is not pre-populated
     * (the create_test_file wrote via write(), not mmap) */
    sc1(SYS_CLOSE, fd);
    fd = sc3(SYS_OPEN, (long)"/tmp/dptest", O_RDONLY, 0);
    check("reopen", fd >= 0);

    /* mmap the file — with demand paging, no pages are loaded yet */
    long addr = sc6(SYS_MMAP, 0, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap file", addr > 0x1000);

    /* First access triggers page fault → fault handler reads from file */
    volatile uint64_t *p = (volatile uint64_t *)addr;
    check_val("demand data[0]", (long)*p, (long)0xFEEDC0DEFEEDC0DEULL);
    check_val("demand data[1]", (long)p[1], (long)0xFEEDC0DEFEEDC0DEULL);

    sc2(SYS_MUNMAP, addr, PAGE);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dptest");
}

/* test_demand_paging_sparse: mmap large file, only touch pages 0 and 7 */
static void test_demand_paging_sparse(void) {
    puts("\n[demand_paging sparse]\n");

    /* Create an 8-page file with page-specific patterns */
    long fd = sc3(SYS_OPEN, (long)"/tmp/dptest_sparse", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check("create file", fd >= 0);

    uint64_t buf[PAGE / 8];
    for (int pg = 0; pg < 8; pg++) {
        for (int i = 0; i < PAGE / 8; i++)
            buf[i] = 0x1000000000000000ULL * pg + (uint64_t)i;
        sc3(SYS_WRITE, fd, (long)buf, PAGE);
    }

    sc1(SYS_CLOSE, fd);
    fd = sc3(SYS_OPEN, (long)"/tmp/dptest_sparse", O_RDONLY, 0);
    check("reopen", fd >= 0);

    long addr = sc6(SYS_MMAP, 0, 8 * PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap 8 pages", addr > 0x1000);

    /* Only touch page 0 and page 7 — intermediate pages never faulted */
    volatile uint64_t *p0 = (volatile uint64_t *)addr;
    volatile uint64_t *p7 = (volatile uint64_t *)(addr + 7 * PAGE);

    check_val("page0[0]", (long)p0[0], (long)0x0000000000000000ULL);
    check_val("page0[1]", (long)p0[1], (long)0x0000000000000001ULL);
    check_val("page7[0]", (long)p7[0], (long)0x7000000000000000ULL);
    check_val("page7[1]", (long)p7[1], (long)0x7000000000000001ULL);

    sc2(SYS_MUNMAP, addr, 8 * PAGE);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dptest_sparse");
}

/* test_demand_paging_shared: MAP_SHARED file-backed demand paging */
static void test_demand_paging_shared(void) {
    puts("\n[demand_paging shared]\n");

    long fd = create_test_file("/tmp/dptest_sh", 0xBEEFCAFEBEEFCAFEULL);
    check("create file", fd >= 0);

    sc1(SYS_CLOSE, fd);
    fd = sc3(SYS_OPEN, (long)"/tmp/dptest_sh", O_RDWR, 0);
    check("reopen", fd >= 0);

    /* First mapping: demand paging loads the page into page cache */
    long addr1 = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap shared 1", addr1 > 0x1000);

    volatile uint64_t *p1 = (volatile uint64_t *)addr1;
    check_val("shared data", (long)*p1, (long)0xBEEFCAFEBEEFCAFEULL);

    /* Second mapping: should reuse page cache (same physical page) */
    long addr2 = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap shared 2", addr2 > 0x1000);

    volatile uint64_t *p2 = (volatile uint64_t *)addr2;
    check_val("shared map2 data", (long)*p2, (long)0xBEEFCAFEBEEFCAFEULL);

    /* Write through map1 visible in map2 */
    *p1 = 0xAAAABBBBCCCCDDDDULL;
    check_val("shared write visible", (long)*p2, (long)0xAAAABBBBCCCCDDDDULL);

    sc2(SYS_MUNMAP, addr1, PAGE);
    sc2(SYS_MUNMAP, addr2, PAGE);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dptest_sh");
}

/* test_msync_write_back: write via MAP_SHARED mmap, msync, verify via read() */
static void test_msync_write_back(void) {
    puts("\n[msync write_back]\n");

    long fd = create_test_file("/tmp/msync_test", 0xAAAAAAAAAAAAAAAAULL);
    check("create file", fd >= 0);
    sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/tmp/msync_test", O_RDWR, 0);
    check("reopen", fd >= 0);

    long addr = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap shared", addr > 0x1000);

    /* Write through mmap */
    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0xDEADBEEFDEADBEEFULL;

    /* msync to flush dirty pages to file */
    long rc = sc3(SYS_MSYNC, addr, PAGE, MS_SYNC);
    check("msync", rc == 0);

    /* Read file via read() to verify write-back */
    sc3(SYS_LSEEK, fd, 0, SEEK_SET);
    uint64_t buf[PAGE / 8];
    long rd = sc3(SYS_READ, fd, (long)buf, PAGE);
    check("read", rd == PAGE);
    check_val("data written back", (long)buf[0], (long)0xDEADBEEFDEADBEEFULL);
    /* Other words should still have original pattern */
    check_val("other data intact", (long)buf[1], (long)0xAAAAAAAAAAAAAAAAULL);

    sc2(SYS_MUNMAP, addr, PAGE);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/msync_test");
}

/* test_dirty_tracking: only written pages are dirty, others stay clean */
static void test_dirty_tracking(void) {
    puts("\n[dirty_tracking]\n");

    /* Create 3-page file with known pattern */
    long fd = sc3(SYS_OPEN, (long)"/tmp/dirty_track", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check("create file", fd >= 0);

    uint64_t buf[PAGE / 8];
    for (int pg = 0; pg < 3; pg++) {
        for (int i = 0; i < PAGE / 8; i++)
            buf[i] = 0x1111111111111111ULL * (uint64_t)(pg + 1);
        sc3(SYS_WRITE, fd, (long)buf, PAGE);
    }
    sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/tmp/dirty_track", O_RDWR, 0);
    check("reopen", fd >= 0);

    long addr = sc6(SYS_MMAP, 0, 3 * PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap 3 pages", addr > 0x1000);

    /* Only write to page 1 (middle page) */
    volatile uint64_t *p1 = (volatile uint64_t *)(addr + PAGE);
    *p1 = 0xBBBBBBBBBBBBBBBBULL;

    /* msync all 3 pages */
    long rc = sc3(SYS_MSYNC, addr, 3 * PAGE, MS_SYNC);
    check("msync", rc == 0);

    /* Read back and verify: page 0 unchanged, page 1 modified, page 2 unchanged */
    sc3(SYS_LSEEK, fd, 0, SEEK_SET);
    uint64_t rbuf[PAGE / 8];

    sc3(SYS_READ, fd, (long)rbuf, PAGE);
    check_val("page0 unchanged", (long)rbuf[0], (long)0x1111111111111111ULL);

    sc3(SYS_READ, fd, (long)rbuf, PAGE);
    check_val("page1 written", (long)rbuf[0], (long)0xBBBBBBBBBBBBBBBBULL);

    sc3(SYS_READ, fd, (long)rbuf, PAGE);
    check_val("page2 unchanged", (long)rbuf[0], (long)0x3333333333333333ULL);

    sc2(SYS_MUNMAP, addr, 3 * PAGE);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/dirty_track");
}

/* test_munmap_implicit_writeback: dirty pages written back on munmap */
static void test_munmap_implicit_writeback(void) {
    puts("\n[munmap_implicit_writeback]\n");

    long fd = create_test_file("/tmp/munmap_wb", 0x5555555555555555ULL);
    check("create file", fd >= 0);
    sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/tmp/munmap_wb", O_RDWR, 0);
    check("reopen", fd >= 0);

    long addr = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap shared", addr > 0x1000);

    /* Write through mmap, no explicit msync */
    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0x9999999999999999ULL;

    /* munmap should implicitly write-back dirty pages */
    sc2(SYS_MUNMAP, addr, PAGE);

    /* Verify via read() */
    sc3(SYS_LSEEK, fd, 0, SEEK_SET);
    uint64_t buf[PAGE / 8];
    long rd = sc3(SYS_READ, fd, (long)buf, PAGE);
    check("read", rd == PAGE);
    check_val("implicit writeback", (long)buf[0], (long)0x9999999999999999ULL);

    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/munmap_wb");
}

TEST("shared_mmap_basic", test_shared_mmap_basic);
TEST("shared_mmap_fork", test_shared_mmap_fork);
TEST("shared_mmap_two_maps", test_shared_mmap_two_maps);
TEST("demand_paging_file", test_demand_paging_file);
TEST("demand_paging_sparse", test_demand_paging_sparse);
TEST("demand_paging_shared", test_demand_paging_shared);
TEST("msync_write_back", test_msync_write_back);
TEST("dirty_tracking", test_dirty_tracking);
TEST("munmap_implicit_writeback", test_munmap_implicit_writeback);
