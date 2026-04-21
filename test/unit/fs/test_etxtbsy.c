/* ETXTBSY (i_writecount) — open(W) on executing binary and exec on writable file.
 *
 * Linux pre-6.11 behavior: deny_write_access while process image active. */
#include "ktest.h"

static void test_etxtbsy_open_vs_open(void) {
    puts("\n[etxtbsy/open-vs-open]\n");

    /* Create a file, hold it open for write, second write-open should succeed
     * (both are writers — no ETXTBSY between writers). */
    long fd1 = sc3(SYS_OPEN, (long)"/tmp/etxt_a", O_CREAT | O_WRONLY, 0644);
    check("create a", fd1 >= 0);
    long fd2 = sc3(SYS_OPEN, (long)"/tmp/etxt_a", O_WRONLY, 0);
    check("second write-open ok", fd2 >= 0);
    if (fd1 >= 0) sc1(SYS_CLOSE, fd1);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);
    sc1(SYS_UNLINK, (long)"/tmp/etxt_a");
}

/* Direct i_writecount semantics via execve: if we exec a binary and a writer
 * has it open, execve → -ETXTBSY; if we open(W) a binary currently exec'd
 * by a different process, open → -ETXTBSY. Testing in ktest requires a
 * cooperating child process; ktest boots single-process, so we simulate
 * the write-side check via the same ktest binary image.
 *
 * Instead, we test the core invariants that are observable without fork:
 *   1. open(W) then execve on same path → execve returns -ETXTBSY
 *      (writecount > 0 at exec time)
 *   2. After close, execve succeeds (writecount == 0)
 * These match the LTP creat07 semantic from the exec side. */
static void test_etxtbsy_execve_writer_held(void) {
    puts("\n[etxtbsy/execve-writer-held]\n");

    /* Seed a binary-like file in /tmp (not a real ELF — execve fails with
     * -ENOEXEC after the ETXTBSY gate. We only check that the ETXTBSY gate
     * fires BEFORE the exec content validation.) */
    long fd = sc3(SYS_OPEN, (long)"/tmp/etxt_bin",
                  O_CREAT | O_WRONLY, 0755);
    check("create bin", fd >= 0);
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)"garbage", 7);

    /* Hold fd open (writable) → exec should see writecount > 0 → -ETXTBSY. */
    char *argv[] = { (char *)"/tmp/etxt_bin", 0 };
    long r = sc3(SYS_EXECVE, (long)"/tmp/etxt_bin", (long)argv, 0);
    check_val("execve with writer held → ETXTBSY", r, -ETXTBSY);

    sc1(SYS_CLOSE, fd);

    /* After close, execve sees writecount == 0 — fails with -ENOEXEC
     * because it's not a valid ELF. ETXTBSY gate should no longer fire. */
    r = sc3(SYS_EXECVE, (long)"/tmp/etxt_bin", (long)argv, 0);
    check("execve after close → != ETXTBSY", r != -ETXTBSY);

    sc1(SYS_UNLINK, (long)"/tmp/etxt_bin");
}

TEST("etxtbsy/open-vs-open",        test_etxtbsy_open_vs_open);
TEST("etxtbsy/execve-writer-held",  test_etxtbsy_execve_writer_held);
