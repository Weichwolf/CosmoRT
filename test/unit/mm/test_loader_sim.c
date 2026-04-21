/* Class D: Dynamic-Linker / Loader-Pattern.
 *
 * musl ld-dynamic-linker macht:
 *   open(libfoo.so) → mmap(PROT_READ) → parse ELF → mmap(PROT_READ|PROT_EXEC)
 *   → mprotect → relocation fixups → munmap scratch.
 * Wir simulieren das in-kernel ohne echten Loader. */
#include "ktest.h"

#define DLOPEN_ITERS       16
#define SMALL_READ_BYTES   4
#define SMALL_READ_COUNT   200

#define PAGE               4096

static void test_mmap_mprotect_exec(void) {
    puts("\n[LOADER_MPROTECT_EXEC]\n");

    for (int i = 0; i < DLOPEN_ITERS; i++) {
        long m = sc6(SYS_MMAP, 0, 2 * PAGE, PROT_READ | PROT_WRITE,
                     MAP_PRIV_ANON, -1, 0);
        if (m <= 0) { check("mmap rw", 0); return; }

        uint8_t *code = (uint8_t *)m;
        code[0] = 0xC3;

        long mp = sc3(SYS_MPROTECT, m, 2 * PAGE, PROT_READ | PROT_EXEC);
        if (mp != 0) { check("mprotect r+x", 0); sc2(SYS_MUNMAP, m, 2 * PAGE); return; }

        ((void (*)(void))m)();

        long mp2 = sc3(SYS_MPROTECT, m, PAGE, PROT_READ | PROT_WRITE);
        if (mp2 != 0) { check("mprotect back r+w", 0); sc2(SYS_MUNMAP, m, 2 * PAGE); return; }

        sc2(SYS_MUNMAP, m, 2 * PAGE);
    }
    check("16 loader cycles completed", 1);
}

static void test_many_small_reads(void) {
    puts("\n[LOADER_MANY_READS]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/loader_scratch",
                  O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open", fd >= 0);
    if (fd < 0) return;

    char blob[1024];
    for (int i = 0; i < 1024; i++) blob[i] = (char)(i & 0xFF);
    sc3(SYS_WRITE, fd, (long)blob, 1024);
    sc1(SYS_CLOSE, fd);

    int ok = 0;
    for (int i = 0; i < SMALL_READ_COUNT; i++) {
        fd = sc3(SYS_OPEN, (long)"/tmp/loader_scratch", O_RDONLY, 0);
        if (fd < 0) break;
        char buf[SMALL_READ_BYTES];
        long n = sc3(SYS_READ, fd, (long)buf, SMALL_READ_BYTES);
        if (n == SMALL_READ_BYTES) ok++;
        sc1(SYS_CLOSE, fd);
    }
    check_val("200× open+read(4)+close", ok, SMALL_READ_COUNT);
    sc1(SYS_UNLINK, (long)"/tmp/loader_scratch");
}

static void test_mmap_populate_map_fixed(void) {
    puts("\n[LOADER_FIXED_MMAP]\n");

    long base = sc6(SYS_MMAP, 0, 8 * PAGE, PROT_NONE,
                    MAP_PRIV_ANON, -1, 0);
    check("reserve VM", base > 0);
    if (base <= 0) return;

    long text = sc6(SYS_MMAP, base, PAGE, PROT_READ | PROT_EXEC,
                    MAP_PRIV_ANON | MAP_FIXED, -1, 0);
    check_val("fixed text segment", text, base);

    long data = sc6(SYS_MMAP, base + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE,
                    MAP_PRIV_ANON | MAP_FIXED, -1, 0);
    check_val("fixed data segment", data, base + 2 * PAGE);

    long bss = sc6(SYS_MMAP, base + 3 * PAGE, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIV_ANON | MAP_FIXED, -1, 0);
    check_val("fixed bss segment", bss, base + 3 * PAGE);

    *(volatile uint32_t *)data = 0x12345678;
    *(volatile uint32_t *)bss = 0;
    check_val("data init", (long)*(volatile uint32_t *)data, 0x12345678);
    check_val("bss zero", (long)*(volatile uint32_t *)bss, 0);

    sc2(SYS_MUNMAP, base, 8 * PAGE);
}

TEST("loader/mprotect_exec",   test_mmap_mprotect_exec);
TEST("loader/many_reads",      test_many_small_reads);
TEST("loader/fixed_mmap",      test_mmap_populate_map_fixed);
