/* CosmoRT Process — address space + FD table + thread container */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "spinlock.h"
#include "thread.h"
#include "fd.h"
#include "vma.h"

/* Process states */
#define PROC_FREE    0
#define PROC_ALIVE   1
#define PROC_ZOMBIE  2

typedef struct process {
    int         state;
    uint32_t    pid;
    uint32_t    parent_pid;
    int         exit_code;

    /* Address space */
    uint64_t   *pml4;

    /* Memory */
    uint64_t    brk_base;
    uint64_t    brk_current;
    uint64_t    mmap_next;

    /* VMA tree */
    vma_t      *vma_root;

    /* mlockall flags (MCL_CURRENT | MCL_FUTURE) */
    int         mlockall_flags;

    /* Signals */
    uint64_t    sig_pending;        /* bitmask of pending signals */
    uint64_t    sig_blocked;        /* bitmask of blocked signals (sigprocmask) */
    void       *sig_handlers[32];   /* SIG_DFL=0, SIG_IGN=1, or handler address */

    /* Working directory */
    char        cwd[256];

    /* File descriptors */
    fd_table_t  fds;

    /* Threads */
    thread_t   *main_thread;
    thread_t   *threads;      /* linked list */
    int         thread_count;

    spinlock_t  lock;
} process_t;

extern process_t proc_pool[PROC_MAX];

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

/* Process fork/exec */
long do_fork(void);
long do_execve(const char *path, char *const argv[], char *const envp[]);
long do_wait4(int pid, int *wstatus, int options, void *rusage);

/* Process cleanup */
void proc_cleanup(process_t *p);
void free_address_space(uint64_t *pml4);

/* Find process by PID */
process_t *proc_find(uint32_t pid);

/* Legacy compat for sched.c — maps to thread operations */
#define proc_return_to_kernel(slot) thread_return_to_kernel(percpu_self()->current_thread)

#endif
