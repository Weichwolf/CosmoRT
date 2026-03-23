/* LTP: mmap/munmap/mprotect/mremap/madvise */
#define _GNU_SOURCE
#include "ltp.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf _jmp;
static volatile int _got_sig;

static void segv_handler(int sig) {
    (void)sig;
    _got_sig = 1;
    siglongjmp(_jmp, 1);
}

int main(void) {
    printf("=== LTP: mmap ===\n");
    long page = sysconf(_SC_PAGESIZE);

    /* 1. MAP_ANONYMOUS basic */
    void *p = mmap(NULL, page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap MAP_ANONYMOUS", p != MAP_FAILED);
    if (p != MAP_FAILED) {
        memset(p, 0xAB, page);
        CHECK("write to anon mmap", ((unsigned char *)p)[0] == 0xAB);
        munmap(p, page);
    }

    /* 2. munmap success */
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int r = munmap(p, page);
    CHECK("munmap returns 0", r == 0);

    /* 3. mmap file-backed */
    const char *tmp = "/tmp/ltp_mmap_test";
    int fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "FILE_DATA_1234", 14);
    p = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK("mmap file-backed", p != MAP_FAILED);
    if (p != MAP_FAILED) {
        CHECK("mmap file content", memcmp(p, "FILE_DATA_1234", 14) == 0);
        munmap(p, page);
    }
    close(fd);
    unlink(tmp);

    /* 4. MAP_SHARED writes visible in file */
    fd = open(tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    ftruncate(fd, page);
    p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK("mmap MAP_SHARED", p != MAP_FAILED);
    if (p != MAP_FAILED) {
        memcpy(p, "SHARED", 6);
        msync(p, page, MS_SYNC);
        char buf[8] = {0};
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, 6);
        CHECK("MAP_SHARED visible in file", memcmp(buf, "SHARED", 6) == 0);
        munmap(p, page);
    }
    close(fd);
    unlink(tmp);

    /* 5. mprotect PROT_NONE → SIGSEGV on access */
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(p, page, PROT_NONE);
    struct sigaction sa = { .sa_handler = segv_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    _got_sig = 0;
    if (sigsetjmp(_jmp, 1) == 0) {
        volatile char x = ((volatile char *)p)[0];
        (void)x;
    }
    CHECK("mprotect PROT_NONE → SIGSEGV", _got_sig == 1);
    signal(SIGSEGV, SIG_DFL);
    munmap(p, page);

    /* 6. mprotect restore READ */
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ((char *)p)[0] = 'Z';
    mprotect(p, page, PROT_NONE);
    mprotect(p, page, PROT_READ);
    CHECK("mprotect restore READ", ((char *)p)[0] == 'Z');
    munmap(p, page);

    /* 7. mmap bad fd → EBADF */
    errno = 0;
    p = mmap(NULL, page, PROT_READ, MAP_PRIVATE, 9999, 0);
    CHECK_ERRNO("mmap bad fd → EBADF", p == MAP_FAILED, EBADF);

    /* 8. mmap zero length → EINVAL */
    errno = 0;
    p = mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK_ERRNO("mmap zero len → EINVAL", p == MAP_FAILED, EINVAL);

    /* 9. mremap grow */
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ((char *)p)[0] = 'M';
    void *p2 = mremap(p, page, page * 2, MREMAP_MAYMOVE);
    CHECK("mremap grow", p2 != MAP_FAILED);
    if (p2 != MAP_FAILED) {
        CHECK("mremap preserves data", ((char *)p2)[0] == 'M');
        munmap(p2, page * 2);
    } else {
        munmap(p, page);
    }

    /* 10. madvise MADV_DONTNEED */
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(p, 0xFF, page);
    r = madvise(p, page, MADV_DONTNEED);
    CHECK("madvise MADV_DONTNEED", r == 0);
    /* after DONTNEED, anon pages read as zero */
    CHECK("MADV_DONTNEED zeroes anon page", ((char *)p)[0] == 0);
    munmap(p, page);

    /* 11. MAP_FIXED at chosen address */
    void *base = mmap(NULL, page * 4, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *fixed = mmap((char *)base + page, page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK("mmap MAP_FIXED", fixed == (char *)base + page);
    munmap(base, page * 4);

    LTP_SUMMARY("mmap");
}
