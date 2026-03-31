/* test: robust futex — reproduces musl pthread_robust failure */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)

#define FUTEX_WAITERS       0x40000000
#define FUTEX_OWNER_DIED    0x40000000
#define FUTEX_TID_MASK      0x3FFFFFFF

/*
 * Linux ABI: robust_list_head. When a thread dies holding a futex
 * registered via set_robust_list(), the kernel walks the list and sets
 * FUTEX_OWNER_DIED on each held futex word.
 * CosmoRT: robust_list is saved but never walked on exit.
 */

struct robust_list {
    struct robust_list *next;
};

struct robust_list_head {
    struct robust_list list;
    long futex_offset;
    struct robust_list *list_op_pending;
};

static void test_robust_futex(void) {
    puts("\n[ipc/robust-futex]\n");

    /* Shared page for futex word visible across fork */
    long page = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", page > 0);
    if (page < 0) return;

    volatile uint32_t *futex = (volatile uint32_t *)page;
    *futex = 0; /* unlocked */

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc2(SYS_MUNMAP, page, 4096); return; }

    if (pid == 0) {
        /* child: register robust list, "lock" futex, exit without unlock */
        long child_tid = sc0(SYS_GETTID);

        /*
         * Build a robust_list_head where the single entry points to a
         * wrapper struct containing both the list link and the futex word.
         * futex_offset tells the kernel how far from the list entry to
         * find the actual futex word.
         *
         * Layout: We place the futex word at the shared page.
         * The robust_list entry is on the stack; futex_offset points
         * from &entry to the shared futex word. But Linux ABI expects
         * futex_offset to be a fixed offset applied to each list entry.
         *
         * Simpler approach: put the list entry right before the futex
         * word in the shared page, so futex_offset = sizeof(struct robust_list).
         */
        struct robust_list *entry = (struct robust_list *)page;
        /* futex word is at page + sizeof(struct robust_list) = page + 8 */
        volatile uint32_t *fw = (volatile uint32_t *)(page + sizeof(struct robust_list));

        struct robust_list_head head;
        head.list.next = entry;     /* first (and only) entry */
        entry->next = &head.list;   /* circular: back to head */
        head.futex_offset = (long)sizeof(struct robust_list); /* offset from entry to futex */
        head.list_op_pending = NULL;

        long r = sc2(SYS_SET_ROBUST_LIST, (long)&head, (long)sizeof(head));
        check_val("set_robust_list", r, 0);

        /* "Lock" the futex: write TID | FUTEX_WAITERS */
        *fw = (uint32_t)(child_tid & FUTEX_TID_MASK) | FUTEX_WAITERS;

        /* Exit without unlocking — kernel should set OWNER_DIED */
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* parent: wait for child exit */
    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check("child exited", WIFEXITED(wstatus));

    /* The futex word is at page + sizeof(struct robust_list) */
    volatile uint32_t *fw = (volatile uint32_t *)(page + sizeof(struct robust_list));
    uint32_t val = *fw;

    puts("  futex word = 0x"); put_hex(val); puts("\n");

    /* Bug: kernel doesn't walk robust list, so OWNER_DIED bit is missing */
    check("FUTEX_OWNER_DIED set", (val & FUTEX_OWNER_DIED) != 0);

    sc2(SYS_MUNMAP, page, 4096);
}

TEST("ipc/robust-futex", test_robust_futex);
