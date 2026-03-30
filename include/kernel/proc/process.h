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

#define PROC_FREE    0
#define PROC_ALIVE   1
#define PROC_ZOMBIE  2

typedef struct process {
    int         state;
    uint32_t    pid;
    uint32_t    parent_pid;
    uint32_t    pgid;
    uint32_t    sid;
    int         exit_code;
    int         exit_signal;

    int         stop_signal;
    int         was_continued;

    int         vfork_parent_tid;

    uint64_t   *pml4;

    uint64_t    brk_base;
    uint64_t    brk_current;
    uint64_t    brk_ceiling;
    uint64_t    mmap_next;

    vma_t      *vma_root;

    int         mlockall_flags;

    int         is_driver;

    uint64_t    sig_pending;
    struct k_sigaction sig_actions[64];
    uint64_t    sig_trampoline_page;

    uint64_t    alarm_deadline_ms;

    char        exe_path[256];

    char        cmdline[1024];
    int         cmdline_len;

    char        comm[16];

    char        cwd[256];

    fd_table_t  fds;

    thread_t   *main_thread;
    thread_t   *threads;
    int         thread_count;

    spinlock_t  lock;

    unsigned long rlim_nofile;
} process_t;

#define PID_TABLE_MAX 4096
#define TID_TABLE_MAX 4096

typedef int (*proc_iter_fn)(process_t *p, void *ctx);
void proc_for_each(proc_iter_fn fn, void *ctx);

int proc_count_alive(void);

void proc_init(void);

int proc_create_elf(const void *elf_data, size_t elf_len);

void thread_run(thread_t *t);

void thread_return_to_kernel(thread_t *t);

process_t *proc_current(void);

thread_t *thread_current(void);

void save_user_state_for_block(thread_t *t, long return_value);

uint64_t *alloc_page(void);
int map_user_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot);
int map_user_huge_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot);

long do_fork(void);
long do_vfork(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls);
long do_execve(const char *path, char *const argv[], char *const envp[]);
long do_wait4(int pid, int *wstatus, int options, void *rusage);

void proc_cleanup(process_t *p);
void free_address_space(uint64_t *pml4);

process_t *proc_find(uint32_t pid);

thread_t *thread_find_by_tid(int tid);

#endif
