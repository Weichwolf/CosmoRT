/* CosmoRT Process — address space + FD table + thread container */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "spinlock.h"
#include "proc/thread.h"
#include "event/fd.h"
#include "mm/vma.h"

/* k_sigaction, SA_* flags — from linux.h (via syscall.h) */

/* Process states */
#define PROC_FREE    0
#define PROC_ALIVE   1
#define PROC_ZOMBIE  2

typedef struct process {
    /* ── Cache-line 0 (bytes 0-63): syscall hot path ──
     * state, pid, pml4 — checked on every syscall.
     * parent_pid, pgid, sid — getpid/setpgid/setsid. */
    int         state;
    uint32_t    pid;
    uint32_t    parent_pid;
    uint32_t    pgid;           /* Process Group ID */
    uint32_t    sid;            /* Session ID */
    int         exit_code;
    int         exit_signal;       /* 0 = normal exit, >0 = killed by signal */

    /* Job control state — consumed by wait4(WUNTRACED/WCONTINUED) */
    int         stop_signal;       /* signal that stopped the process (0 = not stopped) */
    int         was_continued;     /* 1 = continued since last wait4(WCONTINUED) */

    /* vfork: TID of parent thread blocked until this child execs/exits (0 = none) */
    int         vfork_parent_tid;

    /* Address space */
    uint64_t   *pml4;

    /* ── Cache-line 1 (bytes 64+): memory management ── */
    /* Memory */
    uint64_t    brk_base;
    uint64_t    brk_current;
    uint64_t    brk_ceiling;   /* fast-reject: VMA collision boundary (0 = uncached) */
    uint64_t    mmap_next;

    /* VMA tree */
    vma_t      *vma_root;

    /* mlockall flags (MCL_CURRENT | MCL_FUTURE) */
    int         mlockall_flags;

    /* Driver capability: 1 = allowed to use HW primitive syscalls (512-520) */
    int         is_driver;

    /* Signals */
    uint64_t    sig_pending;        /* bitmask of pending signals */
    struct k_sigaction sig_actions[64]; /* per-signal action (0-63, SIGRTMIN=32..SIGRTMAX=63) */
    uint64_t    sig_trampoline_page; /* user-addr of RX trampoline page (0 = not yet allocated) */

    /* alarm(2): per-process SIGALRM timer (ms deadline, 0 = inactive) */
    uint64_t    alarm_deadline_ms;

    /* Executable path (set by execve, read by /proc/self/exe) */
    char        exe_path[256];

    /* Command line (null-separated argv, set by execve, read by /proc/pid/cmdline) */
    char        cmdline[1024];
    int         cmdline_len;  /* total bytes including all nulls */

    /* Thread name (prctl PR_SET_NAME / PR_GET_NAME) */
    char        comm[16];

    /* Working directory */
    char        cwd[256];

    /* File descriptors */
    fd_table_t  fds;

    /* Threads */
    thread_t   *main_thread;
    thread_t   *threads;      /* linked list */
    int         thread_count;

    spinlock_t  lock;

    /* Resource limits (at end to avoid offset shifts) */
    unsigned long rlim_nofile;   /* RLIMIT_NOFILE cur (0 = FD_MAX default) */

    /* Signal to parent on exit (clone exit_signal, 0 = none → fallback SIGCHLD) */
    int         notify_signal;
} process_t;

/* PID/TID lookup table sizes */
#define PID_TABLE_MAX 4096
#define TID_TABLE_MAX 4096

/* Iterate all live processes via pid_table. Callback returns 0 to continue, nonzero to stop. */
typedef int (*proc_iter_fn)(process_t *p, void *ctx);
void proc_for_each(proc_iter_fn fn, void *ctx);

/* Count live processes */
int proc_count_alive(void);

/* Initialize process subsystem (slab pools) */
void proc_init(void);

/* Create process from ELF binary. Returns PID or -1. */
int proc_create_elf(const void *elf_data, size_t elf_len);

/* Run a thread (setjmp + IRET to Ring 3). Returns when thread yields/exits. */
void thread_run(thread_t *t);

/* Return from userspace to kernel main loop (longjmp) */
void thread_return_to_kernel(thread_t *t);

/* Get current process (via percpu → current thread → process) */
process_t *proc_current(void);

/* Get current thread (via percpu) */
thread_t *thread_current(void);

/* Save user register state from syscall frame into thread_t for blocking */
void save_user_state_for_block(thread_t *t, long return_value);

/* Page table helpers */
uint64_t *alloc_page(void);
int map_user_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot);
int map_user_huge_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot);

/* Process fork/exec */
long do_fork(unsigned long flags, void *child_stack,
             int *parent_tid, int *child_tid, unsigned long tls);
long do_vfork(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls);
long do_execve(const char *path, char *const argv[], char *const envp[]);
long do_wait4(int pid, int *wstatus, int options, void *rusage);

/* Process cleanup */
void proc_cleanup(process_t *p);
void free_address_space(uint64_t *pml4);

/* Find process by PID — O(1) via pid_table */
process_t *proc_find(uint32_t pid);

/* Find thread by TID — O(1) via tid_table */
thread_t *thread_find_by_tid(int tid);

#endif
