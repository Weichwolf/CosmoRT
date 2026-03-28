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
#include "fs/ext2.h"
#include "memops.h"
#include "uaccess.h"
#include "sys/syscall.h"
#include "core/irq.h"
#include "core/event_queue.h"
#include "net/socket.h"
#include "arch/arch.h"

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
extern process_t *pid_table[PID_TABLE_MAX];
extern thread_t  *tid_table[TID_TABLE_MAX];
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

/* Internal helpers — defined in process_exec.c */
#define EXECVE_MAX_ARGS  16
#define EXECVE_MAX_ENVS  16
#define EXECVE_MAX_STRLEN ELF_LOAD_MAX_STRLEN
uint64_t build_user_stack(uint64_t *user_pml4, uint64_t stack_top,
                          char kargv[][EXECVE_MAX_STRLEN], int argc,
                          char kenvp[][EXECVE_MAX_STRLEN], int envc,
                          const elf_info_t *elf_info);

/* Internal helpers — defined in process_fork.c */
int copy_address_space(process_t *child, process_t *parent);

#endif
