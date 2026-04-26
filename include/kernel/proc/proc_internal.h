/* CosmoRT Process — internal header shared between process_*.c files.
 * NOT part of the public API. Only #include from src/kernel/proc/. */
#ifndef PROC_INTERNAL_H
#define PROC_INTERNAL_H

#include "proc/process.h"
#include "mm/paging.h"
#include "hw/serial.h"
#include "mm/page_alloc.h"
#include "mm/slab.h"
#include "proc/elf.h"
#include "core/percpu.h"
#include "config.h"
#include "mm/vma.h"
#include "fs/vfs.h"
#include "fs/ext4.h"
#include "memops.h"
#include "uaccess.h"
#include "sys/syscall.h"
#include "core/irq.h"
#include "net/socket.h"
#include "net/net_ns.h"
#include "arch/arch.h"
#include "hal/hal.h"

/* Page table flags */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PS        (1ULL << 7)
#define PTE_COW       (1ULL << 9)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_LAZYFREE  (1ULL << 10)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Shared state — defined in process.c */
extern slab_t proc_slab;
extern slab_t thread_slab;
extern spinlock_t pid_lock;
extern process_t **pid_table;
extern thread_t  **tid_table;
extern uint64_t pml4[];

/* Internal helpers — defined in process.c */
uint64_t read_pte_pub(uint64_t *user_pml4, uint64_t va);
uint64_t prot_to_pte(int prot);
uint64_t *get_or_alloc_level(uint64_t *table, int idx);
uint64_t *create_user_pml4(void);
process_t *proc_alloc(void);
uint64_t aslr_rand(void);

/* Internal helpers — defined in process_lazy.c */
uint64_t read_pte(uint64_t *user_pml4, uint64_t va);
void vma_free_tree(vma_t *node);
void unmap_shared_vmas(vma_t *node, uint64_t *pml4);

/* Internal helpers — defined in process_exec.c.
 * Linux fs/exec.c: MAX_ARG_STRLEN = 32*PAGE_SIZE, MAX_ARG_STRINGS = 0x7FFFFFFF,
 * ARG_MAX total per POSIX (typ. 128KB auf Alpine). Totals sind durch
 * EXECVE_BUF_SIZE gebunden; Overflow liefert -E2BIG (strict POSIX). */
#define EXECVE_MAX_ARGS    4096
#define EXECVE_MAX_ENVS    4096
#define EXECVE_MAX_STRLEN  (128 * 1024)
#define EXECVE_BUF_SIZE    (128 * 1024)
#define EXECVE_BUF_PAGES   (EXECVE_BUF_SIZE / 4096)
uint64_t build_user_stack(uint64_t *user_pml4, uint64_t stack_top,
                          const char *const *argv, int argc,
                          const char *const *envp, int envc,
                          const elf_info_t *elf_info);

/* Internal helpers — defined in process_fork.c */
int copy_address_space(process_t *child, process_t *parent);

#endif
