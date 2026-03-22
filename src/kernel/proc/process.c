/* CosmoRT Process + Thread Management
 *
 * Slab-allocated. Each process starts with one main thread.
 * Scheduler operates on threads, not processes.
 */

#include "process.h"
#include "paging.h"
#include "serial.h"
#include "page_alloc.h"
#include "slab.h"
#include "elf.h"
#include "percpu.h"
#include "config.h"
#include "vma.h"
#include "vfs.h"
#include "cosmofs.h"
#include "memops.h"
#include "syscall.h"
#include "irq.h"
#include "socket.h"

/* Page table flags */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PS      (1ULL << 7)
#define PTE_NX      (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Convert PROT_* flags to x86 PTE flags (leaf entry only) */
static uint64_t prot_to_pte(int prot) {
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

/* ── Slab pools ─────────────────────────────────── */

process_t proc_pool[PROC_MAX];
thread_t  thread_pool[THREAD_MAX];

static slab_t proc_slab;
static slab_t thread_slab;
static int next_pid = 1;
static int next_tid = 1;
static spinlock_t pid_lock = SPINLOCK_INIT;

/* O(1) lookup tables — indexed by PID/TID, entries set at alloc, cleared at free */
#define PID_TABLE_MAX 256
#define TID_TABLE_MAX 512
static process_t *pid_table[PID_TABLE_MAX];
static thread_t  *tid_table[TID_TABLE_MAX];

extern uint64_t pml4[]; /* kernel page table from entry.asm */

void proc_init(void) {
    slab_init(&proc_slab, proc_pool, sizeof(process_t), PROC_MAX);
    slab_init(&thread_slab, thread_pool, sizeof(thread_t), THREAD_MAX);
    serial_puts("proc: init (slab)\n");
}

thread_t *thread_alloc(void) {
    thread_t *t = (thread_t *)slab_alloc(&thread_slab);
    if (t) {
        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        t->tid = next_tid++;
        if (t->tid < TID_TABLE_MAX)
            tid_table[t->tid] = t;
        spin_unlock_irq(&pid_lock, flags);
    }
    return t;
}

void thread_free(thread_t *t) {
    if (!t) return;
    if (t->tid > 0 && t->tid < TID_TABLE_MAX)
        tid_table[t->tid] = 0;
    if (t->kstack)
        pages_free(t->kstack, KSTACK_SIZE / 4096);
    slab_free(&thread_slab, t);
}

static process_t *proc_alloc(void) {
    process_t *p = (process_t *)slab_alloc(&proc_slab);
    if (p) {
        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        p->pid = (uint32_t)next_pid++;
        if (p->pid < PID_TABLE_MAX)
            pid_table[p->pid] = p;
        spin_unlock_irq(&pid_lock, flags);
        p->state = PROC_ALIVE;
    }
    return p;
}

/* ── Page table management ──────────────────────── */

uint64_t *alloc_page(void) {
    return (uint64_t *)page_alloc();
}

/* Create user PML4: lower half empty (user), upper half = kernel direct map */
static uint64_t *create_user_pml4(void) {
    uint64_t *user_pml4 = alloc_page();
    if (!user_pml4) return 0;
    /* Lower half: empty (user space) — alloc_page returns zeroed page */
    /* Upper half: share kernel mappings (direct physical map, no PTE_USER) */
    for (int i = 256; i < 512; i++)
        user_pml4[i] = pml4[i];
    return user_pml4;
}

/* Get or allocate a page table level for user mappings.
 * PTE entries store physical addresses; we convert via phys_to_virt/virt_to_phys. */
static uint64_t *get_or_alloc_level(uint64_t *table, int idx) {
    if (table[idx] & PTE_PRESENT)
        return (uint64_t *)phys_to_virt(table[idx] & PTE_ADDR_MASK);

    uint64_t *new_tbl = alloc_page();
    if (!new_tbl) return 0;
    table[idx] = virt_to_phys(new_tbl) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return new_tbl;
}

int map_user_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot) {
    /* User pages only in lower half */
    if (vaddr >= 0x800000000000ULL) return -1;

    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    int pt_idx   = (vaddr >> 12) & 0x1FF;

    /* Intermediate levels always need WRITE+USER so the CPU can walk them */
    uint64_t *pdpt = get_or_alloc_level(user_pml4, pml4_idx);
    if (!pdpt) return -1;

    uint64_t *pd = get_or_alloc_level(pdpt, pdpt_idx);
    if (!pd) return -1;

    uint64_t *pt = get_or_alloc_level(pd, pd_idx);
    if (!pt) return -1;

    /* Only the leaf PTE restricts access based on prot */
    pt[pt_idx] = phys | prot_to_pte(prot);
    return 0;
}

/* ── FD table init ──────────────────────────────── */

/* Default PTY for init's stdio. Set by vt_init(). */
int fd_default_pty = -1;

void fd_table_init(fd_table_t *fdt) {
    for (int i = 0; i < FD_MAX; i++) fdt->entries[i].type = FD_NONE;
    if (fd_default_pty >= 0) {
        /* VT available: stdin/stdout/stderr → PTY slave (VT0) */
        void *pty = (void *)(uintptr_t)fd_default_pty;
        fdt->entries[0] = (fd_entry_t){FD_PTY_SLAVE, pty, O_RDONLY};
        fdt->entries[1] = (fd_entry_t){FD_PTY_SLAVE, pty, O_WRONLY};
        fdt->entries[2] = (fd_entry_t){FD_PTY_SLAVE, pty, O_WRONLY};
    } else {
        /* No VT (early boot / headless) → serial fallback */
        fdt->entries[0] = (fd_entry_t){FD_SERIAL, 0, O_RDONLY};
        fdt->entries[1] = (fd_entry_t){FD_SERIAL, 0, O_WRONLY};
        fdt->entries[2] = (fd_entry_t){FD_SERIAL, 0, O_WRONLY};
    }
    fdt->max_fd = 3;
}

int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags) {
    for (int i = 0; i < FD_MAX; i++) {
        if (fdt->entries[i].type == FD_NONE) {
            fdt->entries[i] = (fd_entry_t){type, obj, flags};
            if (i >= fdt->max_fd) fdt->max_fd = i + 1;
            return i;
        }
    }
    return -1;
}

int fd_close(fd_table_t *fdt, int fd) {
    if (fd < 0 || fd >= FD_MAX) return -1;
    if (fdt->entries[fd].type == FD_NONE) return -1;
    fdt->entries[fd].type = FD_NONE;
    fdt->entries[fd].obj = 0;
    return 0;
}

fd_entry_t *fd_get(fd_table_t *fdt, int fd) {
    if (fd < 0 || fd >= FD_MAX) return 0;
    if (fdt->entries[fd].type == FD_NONE) return 0;
    return &fdt->entries[fd];
}

/* ── Process creation ───────────────────────────── */

/* ASLR via kernel CSPRNG, fallback to RDTSC before init */
static uint64_t aslr_rand(void) {
    uint64_t r;
    extern int random_get(void *buf, size_t len);
    if (random_get(&r, sizeof(r)) == 0) return r;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Forward declarations */
static void vma_free_tree(vma_t *node);
void free_address_space(uint64_t *user_pml4);

int proc_create_elf(const void *elf_data, size_t elf_len) {
    process_t *p = proc_alloc();
    if (!p) return -1;

    p->pml4 = create_user_pml4();
    if (!p->pml4) goto fail_slab;
    p->vma_root = 0;

    /* ASLR: randomize stack and mmap base */
    uint64_t stack_rand = aslr_rand() & 0xFFF000ULL;
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF */
    uint64_t entry, stack_ptr, brk_end;
    if (elf_load(elf_data, elf_len, p->pml4, stack_top,
                 &entry, &stack_ptr, &brk_end) < 0)
        goto fail_pml4;

    p->brk_base = brk_end;
    p->brk_current = brk_end;
    p->is_driver = (p->pid == 1) ? 1 : 0;
    p->cwd[0] = '/'; p->cwd[1] = '\0';
    fd_table_init(&p->fds);

    /* Create VMA for the stack region */
    uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
    vma_insert(&p->vma_root, stack_bottom, stack_top,
               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

    /* Create VMA for brk region (initially zero-sized, grows on brk calls) */
    if (brk_end > 0)
        vma_insert(&p->vma_root, brk_end, brk_end,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

    /* Create VMAs for ELF segments by scanning the ELF headers */
    if (elf_len >= 64) {
        const uint8_t *data = (const uint8_t *)elf_data;
        uint64_t phoff = *(const uint64_t *)(data + 32);
        uint16_t phentsize = *(const uint16_t *)(data + 54);
        uint16_t phnum = *(const uint16_t *)(data + 56);
        for (int i = 0; i < phnum; i++) {
            const uint8_t *ph = data + phoff + (uint64_t)i * phentsize;
            uint32_t p_type = *(const uint32_t *)ph;
            if (p_type != 1) continue;
            uint64_t p_vaddr = *(const uint64_t *)(ph + 16);
            uint64_t p_memsz = *(const uint64_t *)(ph + 40);
            uint32_t p_flags = *(const uint32_t *)(ph + 4);
            if (p_memsz == 0) continue;
            uint64_t seg_start = p_vaddr & ~0xFFFULL;
            uint64_t seg_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            int prot = 0;
            if (p_flags & 4) prot |= PROT_READ;
            if (p_flags & 2) prot |= PROT_WRITE;
            if (p_flags & 1) prot |= PROT_EXEC;
            vma_insert(&p->vma_root, seg_start, seg_end, prot, MAP_PRIVATE);
        }
    }

    /* Create main thread */
    thread_t *t = thread_alloc();
    if (!t) goto fail_elf;

    t->state = THREAD_RUNNABLE;
    t->rip = entry;
    t->rsp = stack_ptr;
    t->rflags = 0x202;
    t->sched_policy = SCHED_OTHER;
    t->priority = PRIO_MIN;
    t->saved_priority = -1;
    t->fs_base = 0;
    t->cpu_affinity = -1;
    t->timeslice = RR_TIMESLICE;
    t->proc = p;

    /* Kernel stack for this thread */
    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) goto fail_thread;
    t->kstack_top = (uint64_t)(uintptr_t)(t->kstack + KSTACK_SIZE);

    p->main_thread = t;
    p->threads = t;
    p->thread_count = 1;

    serial_puts("proc: pid=");
    serial_putchar('0' + (p->pid % 10));
    serial_puts(" tid=");
    serial_putchar('0' + (t->tid % 10));
    serial_putchar('\n');

    /* Add to scheduler */
    extern void sched_add(thread_t *t);
    sched_add(t);

    return (int)p->pid;

fail_thread:
    thread_free(t);
fail_elf:
    free_address_space(p->pml4);
    p->pml4 = 0;
    vma_free_tree(p->vma_root);
    p->vma_root = 0;
    slab_free(&proc_slab, p);
    return -1;
fail_pml4:
    free_address_space(p->pml4);
    p->pml4 = 0;
    slab_free(&proc_slab, p);
    return -1;
fail_slab:
    slab_free(&proc_slab, p);
    return -1;
}

/* ── Context switching ──────────────────────────── */

extern int kernel_setjmp(uint64_t buf[8]);
extern void kernel_longjmp(uint64_t buf[8], int val) __attribute__((noreturn));
extern void proc_enter_ring3(thread_t *t) __attribute__((noreturn));

__attribute__((noinline, optimize("O0")))
void thread_run(thread_t *t) {
    percpu_t *cpu = percpu_self();

    t->state = THREAD_RUNNING;
    cpu->current_thread = t;

    if (kernel_setjmp(t->jmpbuf) != 0) {
        /* Returned via longjmp — thread yielded/exited/preempted */
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        cpu->current_thread = 0;
        return;
    }

    /* Load process page tables */
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(t->proc->pml4)) : "memory");

    /* Set kernel stack for interrupts/syscalls */
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(t->kstack_top);
    cpu->kernel_rsp = t->kstack_top;

    /* Load thread's FS base (TLS) before entering userspace */
    if (t->fs_base) {
        uint64_t fs = t->fs_base;
        __asm__ volatile("wrmsr" :: "c"(0xC0000100),
                         "a"((uint32_t)fs), "d"((uint32_t)(fs >> 32)));
    }

    /* IRET to Ring 3 (proc_enter_ring3 reads thread_t by offsets) */
    proc_enter_ring3(t);
}

void thread_return_to_kernel(thread_t *t) {
    __asm__ volatile("sti");
    kernel_longjmp(t->jmpbuf, 1);
}

process_t *proc_current(void) {
    thread_t *t = thread_current();
    return t ? t->proc : 0;
}

void proc_yield(void) {
    /* no-op in kernel context */
}

/* ── Process lookup — O(1) via pid_table ─────────── */

process_t *proc_find(uint32_t pid) {
    if (pid == 0 || pid >= PID_TABLE_MAX) return 0;
    process_t *p = pid_table[pid];
    if (p && __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != PROC_FREE && p->pid == pid)
        return p;
    return 0;
}

/* ── Thread lookup — O(1) via tid_table ──────────── */

thread_t *thread_find_by_tid(int tid) {
    if (tid <= 0 || tid >= TID_TABLE_MAX) return 0;
    thread_t *t = tid_table[tid];
    if (t && t->state != THREAD_FREE && t->state != THREAD_DEAD && t->tid == tid)
        return t;
    return 0;
}

/* ── Address space helpers ───────────────────────── */

/* Read PTE for a user virtual address. Returns 0 if not mapped. */
static uint64_t read_pte(uint64_t *user_pml4, uint64_t va) {
    int pml4i = (va >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
    int pdpti = (va >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (va >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    int pti = (va >> 12) & 0x1FF;
    return pt[pti];
}

/* Walk VMA tree, calling fn(vma, ctx) for each node */
static void vma_walk(vma_t *node, void (*fn)(vma_t *, void *), void *ctx) {
    if (!node) return;
    vma_walk(node->left, fn, ctx);
    fn(node, ctx);
    vma_walk(node->right, fn, ctx);
}

/* Copy a single VMA's pages from parent to child */
struct copy_ctx { uint64_t *src_pml4; uint64_t *dst_pml4; vma_t **dst_root; int err; };

static void copy_one_vma(vma_t *v, void *arg) {
    struct copy_ctx *ctx = (struct copy_ctx *)arg;
    if (ctx->err) return;

    /* Insert VMA into child's tree */
    vma_t *cv = vma_insert(ctx->dst_root, v->start, v->end, v->prot, v->flags);
    if (!cv) { ctx->err = 1; return; }

    /* Copy pages */
    for (uint64_t va = v->start; va < v->end; va += 4096) {
        uint64_t pte = read_pte(ctx->src_pml4, va);
        if (!(pte & PTE_PRESENT)) continue;

        uint64_t *new_page = alloc_page();
        if (!new_page) { ctx->err = 1; return; }

        /* Copy content from parent page */
        uint64_t src_phys = pte & PTE_ADDR_MASK;
        void *src = phys_to_virt(src_phys);
        kmemcpy(new_page, src, 4096);

        if (map_user_page(ctx->dst_pml4, va, virt_to_phys(new_page), v->prot) < 0) {
            page_free(new_page);
            ctx->err = 1;
            return;
        }
    }
}

static int copy_address_space(process_t *child, process_t *parent) {
    child->pml4 = create_user_pml4();
    if (!child->pml4) return -1;

    struct copy_ctx ctx = {
        .src_pml4 = parent->pml4,
        .dst_pml4 = child->pml4,
        .dst_root = &child->vma_root,
        .err = 0
    };
    vma_walk(parent->vma_root, copy_one_vma, &ctx);
    return ctx.err ? -1 : 0;
}

/* ── Free address space ──────────────────────────── */

/* Free all user pages and page table pages under a PML4 */
void free_address_space(uint64_t *user_pml4) {
    if (!user_pml4) return;

    /* Flush TLB on other cores that may have this PML4 cached */
    tlb_shootdown(virt_to_phys(user_pml4));

    /* Walk lower half only (PML4[0..255] = user space) */
    for (int i = 0; i < 256; i++) {
        if (!(user_pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[i] & PTE_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        page_free(phys_to_virt(pt[l] & PTE_ADDR_MASK));
                    }
                }
                page_free(pt); /* free PT page */
            }
            page_free(pd); /* free PD page */
        }
        page_free(pdpt); /* free PDPT page */
    }
    page_free(user_pml4); /* free PML4 page */
}

/* Free all VMAs in a tree */
static void vma_free_tree(vma_t *node) {
    if (!node) return;
    vma_free_tree(node->left);
    vma_free_tree(node->right);
    vma_free(node);
}

/* ── Process cleanup (2.3) ───────────────────────── */

void proc_cleanup(process_t *p) {
    if (!p) return;

    /* Close all FDs — decrement refcount, free when last ref */
    for (int i = 0; i < FD_MAX; i++) {
        int type = p->fds.entries[i].type;
        if (type == FD_FILE) {
            vfs_file_free_obj(p->fds.entries[i].obj);
        } else if (type != FD_NONE && type != FD_SERIAL) {
            fd_cleanup_entry(type, p->fds.entries[i].obj);
        }
        p->fds.entries[i].type = FD_NONE;
        p->fds.entries[i].obj = 0;
    }

    /* Free all threads */
    thread_t *t = p->threads;
    while (t) {
        thread_t *next = t->proc_next;
        t->state = THREAD_DEAD;
        thread_free(t);
        t = next;
    }
    p->threads = 0;
    p->main_thread = 0;
    p->thread_count = 0;

    /* Free address space (pages + page tables) */
    free_address_space(p->pml4);
    p->pml4 = 0;

    /* Free VMAs */
    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    /* Clear lookup table entry */
    if (p->pid < PID_TABLE_MAX)
        pid_table[p->pid] = 0;

    /* Free process struct */
    slab_free(&proc_slab, p);
}

/* ── fork (2.2) ──────────────────────────────────── */

extern void sched_add(thread_t *t);

long do_fork(void) {
    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || !cur->proc) return -EFAULT;
    process_t *parent = cur->proc;

    /* Allocate child process */
    process_t *child = proc_alloc();
    if (!child) return -ENOMEM;
    child->parent_pid = parent->pid;
    child->vma_root = 0;

    /* Stop other parent threads during page copy to prevent stale data.
     * Use saved_priority as a marker: set to -2 for threads we suspend.
     * Suspend RUNNABLE threads directly. For RUNNING threads on other cores,
     * mark them and send IPI to force them off-CPU before copying. */
    int need_ipi = 0;
    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && (t->state == THREAD_RUNNABLE || t->state == THREAD_RUNNING)) {
            t->state = THREAD_BLOCKED;
            t->saved_priority = -2; /* mark: we stopped this one */
            if (t->cpu_affinity >= 0) need_ipi = 1;
            else need_ipi = 1; /* any RUNNING thread may be on another core */
        }
    }
    if (need_ipi) {
        /* Send IPI to all other cores and wait for them to deschedule.
         * The TLB shootdown vector (0xFE) forces CR3 reload.
         * After IPI + pause loop, suspended threads are no longer executing. */
        extern void tlb_shootdown(uint64_t pml4_phys);
        tlb_shootdown(virt_to_phys(parent->pml4));
    }

    /* Deep-copy address space */
    int copy_err = copy_address_space(child, parent);

    /* Resume only threads we stopped (saved_priority == -2) */
    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && t->saved_priority == -2) {
            t->saved_priority = -1;
            sched_add(t); /* sets THREAD_RUNNABLE and enqueues */
        }
    }

    if (copy_err < 0) {
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }

    child->brk_base = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next = parent->mmap_next;
    child->mlockall_flags = parent->mlockall_flags;
    child->is_driver = parent->is_driver;
    for (int ci = 0; ci < 256; ci++) {
        child->cwd[ci] = parent->cwd[ci];
        if (!parent->cwd[ci]) break;
    }

    /* Inherit signal state */
    child->sig_pending = 0; /* child starts with no pending signals */
    child->sig_blocked = parent->sig_blocked;
    for (int si = 0; si < 32; si++)
        child->sig_actions[si] = parent->sig_actions[si];

    /* Duplicate fd_table — increment refcount on all shared FD objects */
    for (int i = 0; i < FD_MAX; i++) {
        child->fds.entries[i] = parent->fds.entries[i];
        if (parent->fds.entries[i].obj) {
            int ftype = parent->fds.entries[i].type;
            if (ftype == FD_FILE)
                vfs_file_incref((struct vfs_file *)parent->fds.entries[i].obj);
            else if (ftype == FD_PIPE)
                fd_obj_incref(ftype, parent->fds.entries[i].obj);
            else if (ftype == FD_SOCKET || ftype == FD_EPOLL ||
                     ftype == FD_EVENTFD || ftype == FD_TIMERFD ||
                     ftype == FD_INOTIFY)
                fd_obj_incref(ftype, parent->fds.entries[i].obj);
        }
    }
    child->fds.max_fd = parent->fds.max_fd;

    /* Create child thread with parent's saved registers */
    thread_t *ct = thread_alloc();
    if (!ct) {
        free_address_space(child->pml4);
        child->pml4 = 0;
        vma_free_tree(child->vma_root);
        child->vma_root = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }

    /* Save parent's user state */
    save_user_state_for_block(cur, 0);

    /* Copy register state — child gets RAX=0 (fork returns 0 in child) */
    ct->rip = cur->rip;
    ct->rsp = cur->rsp;
    ct->rflags = cur->rflags;
    ct->rax = 0;
    ct->rbx = cur->rbx;
    ct->rcx = cur->rcx;
    ct->rdx = cur->rdx;
    ct->rsi = cur->rsi;
    ct->rdi = cur->rdi;
    ct->rbp = cur->rbp;
    ct->r8  = cur->r8;
    ct->r9  = cur->r9;
    ct->r10 = cur->r10;
    ct->r11 = cur->r11;
    ct->r12 = cur->r12;
    ct->r13 = cur->r13;
    ct->r14 = cur->r14;
    ct->r15 = cur->r15;
    ct->fs_base = cur->fs_base;
    ct->sched_policy = cur->sched_policy;
    ct->priority = cur->priority;
    ct->saved_priority = -1;
    ct->cpu_affinity = -1;
    ct->timeslice = RR_TIMESLICE;
    ct->proc = child;
    ct->state = THREAD_RUNNABLE;

    /* Kernel stack */
    ct->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!ct->kstack) {
        thread_free(ct);
        free_address_space(child->pml4);
        child->pml4 = 0;
        vma_free_tree(child->vma_root);
        child->vma_root = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }
    ct->kstack_top = (uint64_t)(uintptr_t)(ct->kstack + KSTACK_SIZE);

    child->main_thread = ct;
    child->threads = ct;
    child->thread_count = 1;

    /* Add child thread to scheduler */
    sched_add(ct);

    return (long)child->pid;
}

/* ── execve (2.2) ────────────────────────────────── */

/* Copy user path string to kernel buffer (same as in syscall.c).
 * Duplicated here because process.c is a separate compilation unit. */
#define PATH_MAX_PROC 4096
static int copy_path_from_user_proc(char *kbuf, const char *upath, size_t max) {
    if ((uint64_t)upath >= 0x800000000000ULL) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
        if ((uint64_t)(upath + i) >= 0x800000000000ULL) return -EFAULT;
        kbuf[i] = upath[i];
        if (kbuf[i] == '\0') return (int)i;
    }
    return -36; /* ENAMETOOLONG */
}

/* Max entries and string length for execve argv/envp */
#define EXECVE_MAX_ARGS  16
#define EXECVE_MAX_ENVS  16
#define EXECVE_MAX_STRLEN 256

/* Build user stack with argv, envp, auxv. Allocates stack pages.
 * Returns RSP value on success, 0 on error. */
static uint64_t build_user_stack(uint64_t *user_pml4, uint64_t stack_top,
                                 char kargv[][EXECVE_MAX_STRLEN], int argc,
                                 char kenvp[][EXECVE_MAX_STRLEN], int envc,
                                 const elf_info_t *elf_info) {
    /* Map 4 stack pages (16KB) */
    for (int i = 0; i < 4; i++) {
        uint64_t va = stack_top - (uint64_t)(i + 1) * 4096;
        uint64_t *pg = alloc_page();
        if (!pg) return 0;
        if (map_user_page(user_pml4, va, virt_to_phys(pg), PROT_READ | PROT_WRITE) < 0)
            return 0;
    }

    /* Allocate the stack-top page we can write to (overwrites previous mapping) */
    uint64_t stk_page_va = stack_top - 4096;
    uint64_t *frame_page = alloc_page();
    if (!frame_page) return 0;
    map_user_page(user_pml4, stk_page_va, virt_to_phys(frame_page), PROT_READ | PROT_WRITE);
    uint8_t *page = (uint8_t *)frame_page;

    /* Write strings at the top of the page */
    uint64_t str_off = 4096;
    uint64_t argv_addrs[EXECVE_MAX_ARGS];
    uint64_t envp_addrs[EXECVE_MAX_ENVS];

    /* 16 random bytes for AT_RANDOM */
    str_off -= 16;
    str_off &= ~7ULL;
    uint64_t at_random_addr = stk_page_va + str_off;
    extern int random_get(void *buf, size_t len);
    if (random_get(page + str_off, 16) != 0) {
        /* Fallback: zero-fill (better than nothing) */
        kmemset(page + str_off, 0x42, 16);
    }

    /* Environment strings — with bounds checking to prevent underflow */
    for (int i = envc - 1; i >= 0; i--) {
        int sl = 0; while (kenvp[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) return 0; /* not enough space */
        str_off -= (uint64_t)(sl + 1);
        kmemcpy(page + str_off, kenvp[i], (size_t)(sl + 1));
        envp_addrs[i] = stk_page_va + str_off;
    }
    /* Argument strings — with bounds checking to prevent underflow */
    for (int i = argc - 1; i >= 0; i--) {
        int sl = 0; while (kargv[i][sl]) sl++;
        if (str_off < (uint64_t)(sl + 1) + 256) return 0; /* not enough space */
        str_off -= (uint64_t)(sl + 1);
        kmemcpy(page + str_off, kargv[i], (size_t)(sl + 1));
        argv_addrs[i] = stk_page_va + str_off;
    }

    str_off &= ~7ULL;

    /* Count qwords: argc(1) + argv(argc+1) + envp(envc+1) + auxv(8*2+2) */
    int naux = 8; /* PHDR, PHENT, PHNUM, BASE, ENTRY, PAGESZ, RANDOM, NULL */
    int nqwords = 1 + (argc + 1) + (envc + 1) + (naux * 2);
    str_off -= (uint64_t)nqwords * 8;
    str_off &= ~0xFULL; /* 16-byte align RSP at process entry */

    uint64_t *stk = (uint64_t *)(page + str_off);
    uint64_t *sp_base = stk;

    /* argc */
    *stk++ = (uint64_t)argc;
    /* argv pointers */
    for (int i = 0; i < argc; i++)
        *stk++ = argv_addrs[i];
    *stk++ = 0; /* argv terminator */
    /* envp pointers */
    for (int i = 0; i < envc; i++)
        *stk++ = envp_addrs[i];
    *stk++ = 0; /* envp terminator */
    /* auxv */
    *stk++ = AT_PHDR;   *stk++ = elf_info->prog_phdr;
    *stk++ = AT_PHENT;  *stk++ = (uint64_t)elf_info->prog_phent;
    *stk++ = AT_PHNUM;  *stk++ = (uint64_t)elf_info->prog_phnum;
    *stk++ = AT_BASE;   *stk++ = elf_info->interp_base;
    *stk++ = AT_ENTRY;  *stk++ = elf_info->prog_entry;
    *stk++ = AT_PAGESZ; *stk++ = 4096;
    *stk++ = AT_RANDOM; *stk++ = at_random_addr;
    *stk++ = AT_NULL;   *stk++ = 0;

    uint64_t sp = stk_page_va + (uint64_t)((uint8_t *)sp_base - page);
    return sp;
}

/* Create VMAs for mapped ELF segments (using elf_info_t metadata).
 * base is the load base used (0 for ET_EXEC). */
static void create_elf_vmas(vma_t **vma_root, const void *elf_data,
                            size_t elf_len, uint64_t base) {
    if (elf_len < 64) return;
    const uint8_t *data = (const uint8_t *)elf_data;
    uint64_t phoff = *(const uint64_t *)(data + 32);
    uint16_t phentsize = *(const uint16_t *)(data + 54);
    uint16_t phnum = *(const uint16_t *)(data + 56);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = data + phoff + (uint64_t)i * phentsize;
        uint32_t p_type = *(const uint32_t *)ph;
        if (p_type != 1) continue; /* PT_LOAD */
        uint64_t p_vaddr = *(const uint64_t *)(ph + 16) + base;
        uint64_t p_memsz = *(const uint64_t *)(ph + 40);
        uint32_t p_flags = *(const uint32_t *)(ph + 4);
        if (p_memsz == 0) continue;
        uint64_t seg_start = p_vaddr & ~0xFFFULL;
        uint64_t seg_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
        int prot = 0;
        if (p_flags & 4) prot |= PROT_READ;
        if (p_flags & 2) prot |= PROT_WRITE;
        if (p_flags & 1) prot |= PROT_EXEC;
        vma_insert(vma_root, seg_start, seg_end, prot, MAP_PRIVATE);
    }
}

long do_execve(const char *path, char *const argv[], char *const envp[]) {
    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *p = cur->proc;

    /* Copy path to kernel buffer before using it */
    char kpath[PATH_MAX_PROC];
    int plen = copy_path_from_user_proc(kpath, path, PATH_MAX_PROC);
    if (plen < 0) return -EFAULT;

    /* Copy argv/envp from userspace before destroying address space */
    char kargv[EXECVE_MAX_ARGS][EXECVE_MAX_STRLEN];
    int argc = 0;
    if (argv && (uint64_t)argv < 0x800000000000ULL) {
        for (int i = 0; i < EXECVE_MAX_ARGS; i++) {
            char *const *ap = &argv[i];
            if ((uint64_t)ap + sizeof(char *) > 0x800000000000ULL) break;
            char *arg = *ap;
            if (!arg) break;
            if ((uint64_t)arg >= 0x800000000000ULL) break;
            int r = copy_path_from_user_proc(kargv[argc], arg, EXECVE_MAX_STRLEN);
            if (r < 0) break;
            argc++;
        }
    }
    if (argc == 0) {
        int ci = 0;
        while (ci < EXECVE_MAX_STRLEN - 1 && kpath[ci]) { kargv[0][ci] = kpath[ci]; ci++; }
        kargv[0][ci] = '\0';
        argc = 1;
    }

    char kenvp[EXECVE_MAX_ENVS][EXECVE_MAX_STRLEN];
    int envc = 0;
    if (envp && (uint64_t)envp < 0x800000000000ULL) {
        for (int i = 0; i < EXECVE_MAX_ENVS; i++) {
            char *const *ep = &envp[i];
            if ((uint64_t)ep + sizeof(char *) > 0x800000000000ULL) break;
            char *env = *ep;
            if (!env) break;
            if ((uint64_t)env >= 0x800000000000ULL) break;
            int r = copy_path_from_user_proc(kenvp[envc], env, EXECVE_MAX_STRLEN);
            if (r < 0) break;
            envc++;
        }
    }

    /* Determine source: ramfs (small, in-memory) vs CosmoFS (potentially large) */
    uint64_t cosmofs_ino = vfs_cosmofs_lookup(kpath);
    int from_cosmofs = (cosmofs_ino != 0);

    if (from_cosmofs) {
        serial_puts("execve: "); serial_puts(kpath);
        serial_puts(" (cosmofs)\n");
    }

    /* For ramfs files, load into buffer (small embedded binaries) */
    uint8_t *elf_buf = 0;
    size_t elf_len = 0;
    int elf_pages = 0;

    if (!from_cosmofs) {
        struct vfs_node *node = vfs_lookup(kpath);
        if (!node) return -ENOENT;
        if (node->type != VFS_FILE) return -EACCES;
        if (!node->data || node->size == 0) return -ENOEXEC;

        elf_len = node->size;
        elf_pages = (int)((elf_len + 4095) / 4096);
        elf_buf = (uint8_t *)pages_alloc(elf_pages);
        if (!elf_buf) return -ENOMEM;
        kmemcpy(elf_buf, node->data, elf_len);
    }

    /* Read ELF header to determine type (small read for CosmoFS) */
    Elf64_Ehdr peek_eh_buf;
    const Elf64_Ehdr *peek_eh;
    if (from_cosmofs) {
        int hdr_rc = cosmofs_read(cosmofs_ino, &peek_eh_buf, 0, sizeof(peek_eh_buf));
        if (hdr_rc < (int)sizeof(peek_eh_buf))
            return -ENOEXEC;
        peek_eh = &peek_eh_buf;
    } else {
        peek_eh = (const Elf64_Ehdr *)elf_buf;
    }

    /* Determine if dynamic and check for PT_INTERP */
    int has_interp = 0;
    uint8_t *interp_buf = 0;
    size_t interp_len = 0;
    int interp_pages = 0;

    if (peek_eh->e_type == ET_DYN || peek_eh->e_type == ET_EXEC) {
        /* Scan phdrs for PT_INTERP */
        for (int i = 0; i < peek_eh->e_phnum && i < 64; i++) {
            Elf64_Phdr ph;
            size_t phoff = (size_t)(peek_eh->e_phoff + (uint64_t)i * peek_eh->e_phentsize);
            if (from_cosmofs) {
                if (cosmofs_read(cosmofs_ino, &ph, phoff, sizeof(ph)) < (int)sizeof(ph))
                    break;
            } else {
                if (phoff + sizeof(ph) > elf_len) break;
                kmemcpy(&ph, elf_buf + phoff, sizeof(ph));
            }
            if (ph.p_type == PT_INTERP) {
                has_interp = 1;
                char ipath[256];
                size_t iplen = ph.p_filesz;
                if (iplen >= sizeof(ipath)) iplen = sizeof(ipath) - 1;
                if (from_cosmofs) {
                    cosmofs_read(cosmofs_ino, ipath, (size_t)ph.p_offset, iplen);
                } else if (ph.p_offset + iplen <= elf_len) {
                    kmemcpy(ipath, elf_buf + ph.p_offset, iplen);
                }
                ipath[iplen] = '\0';
                while (iplen > 0 && ipath[iplen - 1] == '\0') iplen--;

                int irc = vfs_read_file(ipath, &interp_buf, &interp_len);
                if (irc == 0)
                    interp_pages = (int)((interp_len + 4095) / 4096);
                break;
            }
        }
    }

    /* Switch to kernel PML4 before freeing current address space
     * (we're currently running with p->pml4 in CR3) */
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");

    /* Free current address space */
    free_address_space(p->pml4);
    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    /* Create new PML4 */
    p->pml4 = create_user_pml4();
    if (!p->pml4) {
        if (elf_buf) pages_free(elf_buf, elf_pages);
        if (interp_buf) pages_free(interp_buf, interp_pages);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(cur);
        return -ENOMEM;
    }

    /* ASLR */
    uint64_t stack_rand = aslr_rand() & 0xFFF000ULL;
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    uint64_t entry, stack_ptr;
    int use_ex = (peek_eh->e_type == ET_DYN || has_interp);

    if (use_ex) {
        /* Extended path: ET_DYN and/or PT_INTERP */
        elf_info_t info;
        int load_rc;
        if (from_cosmofs)
            load_rc = elf_load_ex_cosmofs(cosmofs_ino, p->pml4, 0, &info);
        else
            load_rc = elf_load_ex(elf_buf, elf_len, p->pml4, 0, &info);

        if (load_rc < 0) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
            thread_return_to_kernel(cur);
            return -ENOEXEC;
        }

        /* Create VMAs (skip for CosmoFS — segments already mapped) */
        if (!from_cosmofs)
            create_elf_vmas(&p->vma_root, elf_buf, elf_len, info.load_base);

        /* Load interpreter if present */
        if (has_interp && interp_buf) {
            uint64_t interp_base_hint = (info.brk + 0x200000ULL) & ~0xFFFULL;
            elf_info_t interp_info;
            if (elf_load_ex(interp_buf, interp_len, p->pml4,
                            interp_base_hint, &interp_info) < 0) {
                serial_puts("execve: failed to load interpreter\n");
            } else {
                info.interp_base = interp_info.load_base;
                info.entry = interp_info.prog_entry;
                create_elf_vmas(&p->vma_root, interp_buf, interp_len,
                                interp_info.load_base);
            }
        }

        p->brk_base = info.brk;
        p->brk_current = info.brk;
        entry = info.entry;

        uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
        vma_insert(&p->vma_root, stack_bottom, stack_top,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        if (info.brk > 0)
            vma_insert(&p->vma_root, info.brk, info.brk,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

        stack_ptr = build_user_stack(p->pml4, stack_top,
                                     kargv, argc, kenvp, envc, &info);
        if (!stack_ptr) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
            thread_return_to_kernel(cur);
            return -ENOMEM;
        }
    } else {
        /* Fast path: plain ET_EXEC, no interpreter */
        uint64_t brk_end;
        int load_rc;
        if (from_cosmofs)
            load_rc = elf_load_cosmofs(cosmofs_ino, p->pml4, stack_top,
                                       &entry, &stack_ptr, &brk_end);
        else
            load_rc = elf_load(elf_buf, elf_len, p->pml4, stack_top,
                               &entry, &stack_ptr, &brk_end);

        if (load_rc < 0) {
            if (elf_buf) pages_free(elf_buf, elf_pages);
            if (interp_buf) pages_free(interp_buf, interp_pages);
            p->state = PROC_ZOMBIE;
            cur->state = THREAD_DEAD;
            __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
            thread_return_to_kernel(cur);
            return -ENOEXEC;
        }

        p->brk_base = brk_end;
        p->brk_current = brk_end;

        /* Stack VMA */
        uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
        vma_insert(&p->vma_root, stack_bottom, stack_top,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        if (brk_end > 0)
            vma_insert(&p->vma_root, brk_end, brk_end,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

        /* Rebuild stack with real argv/envp (elf_load already set up minimal stack) */
        {
            uint64_t stk_page_va = stack_top - 4096;
            uint64_t pte = read_pte(p->pml4, stk_page_va);
            if (pte & PTE_PRESENT) {
                uint8_t *page = (uint8_t *)phys_to_virt(pte & PTE_ADDR_MASK);
                kmemset(page, 0, 4096);

                uint64_t str_off = 4096;
                uint64_t argv_addrs[EXECVE_MAX_ARGS];
                uint64_t envp_addrs[EXECVE_MAX_ENVS];

                for (int i = envc - 1; i >= 0; i--) {
                    int sl = 0; while (kenvp[i][sl]) sl++;
                    if (str_off < (uint64_t)(sl + 1) + 256) break;
                    str_off -= (uint64_t)(sl + 1);
                    kmemcpy(page + str_off, kenvp[i], (size_t)(sl + 1));
                    envp_addrs[i] = stk_page_va + str_off;
                }
                for (int i = argc - 1; i >= 0; i--) {
                    int sl = 0; while (kargv[i][sl]) sl++;
                    if (str_off < (uint64_t)(sl + 1) + 256) break;
                    str_off -= (uint64_t)(sl + 1);
                    kmemcpy(page + str_off, kargv[i], (size_t)(sl + 1));
                    argv_addrs[i] = stk_page_va + str_off;
                }

                /* Place 16 bytes of random data on stack for AT_RANDOM */
                str_off -= 16;
                str_off &= ~7ULL;
                uint64_t at_random_addr = stk_page_va + str_off;
                {
                    extern int random_get(void *buf, size_t len);
                    uint8_t rand_buf[16];
                    if (random_get(rand_buf, 16) != 0) {
                        /* Fallback: use RDTSC */
                        uint64_t t; __asm__ volatile("rdtsc" : "=A"(t));
                        kmemcpy(rand_buf, &t, 8);
                        kmemcpy(rand_buf + 8, &t, 8);
                    }
                    kmemcpy(page + str_off, rand_buf, 16);
                }

                str_off &= ~7ULL;

                /* Count total qwords needed for the stack frame:
                 * argc(1) + argv(argc+1) + envp(envc+1) + auxv(2*N+2)
                 * auxv entries: AT_ENTRY, AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_RANDOM, AT_NULL = 7 */
                int naux = 7;
                int nqwords = 1 + (argc + 1) + (envc + 1) + (naux * 2);
                /* Align: total frame must land on 16-byte boundary */
                str_off -= (uint64_t)nqwords * 8;
                str_off &= ~0xFULL;  /* 16-byte align RSP at entry */

                uint64_t *stk = (uint64_t *)(page + str_off);
                uint64_t *sp_base = stk;

                /* argc */
                *stk++ = (uint64_t)argc;
                /* argv pointers */
                for (int i = 0; i < argc; i++)
                    *stk++ = argv_addrs[i];
                *stk++ = 0;  /* argv terminator */
                /* envp pointers */
                for (int i = 0; i < envc; i++)
                    *stk++ = envp_addrs[i];
                *stk++ = 0;  /* envp terminator */

                /* Compute AT_PHDR */
                uint64_t phdr_vaddr = peek_eh->e_phoff;
                if (from_cosmofs) {
                    Elf64_Phdr ph0;
                    for (int pi = 0; pi < peek_eh->e_phnum && pi < 64; pi++) {
                        size_t po = (size_t)(peek_eh->e_phoff + (uint64_t)pi * peek_eh->e_phentsize);
                        cosmofs_read(cosmofs_ino, &ph0, po, sizeof(ph0));
                        if (ph0.p_type == PT_LOAD) {
                            phdr_vaddr = ph0.p_vaddr + (peek_eh->e_phoff - ph0.p_offset);
                            break;
                        }
                    }
                } else if (elf_buf && elf_len >= sizeof(Elf64_Ehdr)) {
                    for (int pi = 0; pi < peek_eh->e_phnum && pi < 64; pi++) {
                        size_t po = (size_t)(peek_eh->e_phoff + (uint64_t)pi * peek_eh->e_phentsize);
                        if (po + sizeof(Elf64_Phdr) > elf_len) break;
                        const Elf64_Phdr *ph0 = (const Elf64_Phdr *)(elf_buf + po);
                        if (ph0->p_type == PT_LOAD) {
                            phdr_vaddr = ph0->p_vaddr + (peek_eh->e_phoff - ph0->p_offset);
                            break;
                        }
                    }
                }

                /* auxv (key, value pairs) */
                *stk++ = AT_ENTRY;      *stk++ = entry;
                *stk++ = AT_PHDR;       *stk++ = phdr_vaddr;
                *stk++ = AT_PHENT;      *stk++ = (uint64_t)peek_eh->e_phentsize;
                *stk++ = AT_PHNUM;      *stk++ = (uint64_t)peek_eh->e_phnum;
                *stk++ = AT_PAGESZ;     *stk++ = 4096;
                *stk++ = AT_RANDOM;     *stk++ = at_random_addr;
                *stk++ = AT_NULL;       *stk++ = 0;

                stack_ptr = stk_page_va + (uint64_t)((uint8_t *)sp_base - page);
            }
        }

        /* Create VMAs for ELF segments */
        if (!from_cosmofs) {
            create_elf_vmas(&p->vma_root, elf_buf, elf_len, 0);
        } else {
            /* Create VMAs from CosmoFS ELF phdrs */
            Elf64_Phdr cos_phdrs[64];
            size_t cos_phdr_size = (size_t)peek_eh->e_phnum * peek_eh->e_phentsize;
            cosmofs_read(cosmofs_ino, cos_phdrs, (size_t)peek_eh->e_phoff, cos_phdr_size);
            for (int i = 0; i < peek_eh->e_phnum && i < 64; i++) {
                if (cos_phdrs[i].p_type != PT_LOAD || cos_phdrs[i].p_memsz == 0) continue;
                uint64_t seg_start = cos_phdrs[i].p_vaddr & ~0xFFFULL;
                uint64_t seg_end = (cos_phdrs[i].p_vaddr + cos_phdrs[i].p_memsz + 0xFFF) & ~0xFFFULL;
                int seg_prot = 0;
                if (cos_phdrs[i].p_flags & PF_R) seg_prot |= PROT_READ;
                if (cos_phdrs[i].p_flags & PF_W) seg_prot |= PROT_WRITE;
                if (cos_phdrs[i].p_flags & PF_X) seg_prot |= PROT_EXEC;
                vma_insert(&p->vma_root, seg_start, seg_end, seg_prot, MAP_PRIVATE);
            }
        }
    }

    if (elf_buf) pages_free(elf_buf, elf_pages);
    if (interp_buf) pages_free(interp_buf, interp_pages);

    /* Close O_CLOEXEC fds */
    for (int i = 0; i < FD_MAX; i++) {
        if (p->fds.entries[i].type != FD_NONE &&
            (p->fds.entries[i].flags & 0x80000)) { /* O_CLOEXEC */
            fd_close(&p->fds, i);
        }
    }

    /* Set up thread for new execution */
    cur->rip = entry;
    cur->rsp = stack_ptr;
    cur->rflags = 0x202;
    cur->rax = 0;
    cur->rbx = 0; cur->rcx = 0; cur->rdx = 0;
    cur->rsi = 0; cur->rdi = 0; cur->rbp = 0;
    cur->r8  = 0; cur->r9  = 0; cur->r10 = 0;
    cur->r11 = 0; cur->r12 = 0; cur->r13 = 0;
    cur->r14 = 0; cur->r15 = 0;
    cur->fs_base = 0;

    serial_puts("execve: entering ring3 entry=");
    serial_hex64(entry);
    serial_puts(" sp=");
    serial_hex64(stack_ptr);
    serial_putchar('\n');

    /* Load new page tables and jump to userspace */
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(p->pml4)) : "memory");

    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(cur->kstack_top);
    percpu_self()->kernel_rsp = cur->kstack_top;

    proc_enter_ring3(cur);
    /* unreachable */
    return 0;
}

/* ── wait4 (2.2) ─────────────────────────────────── */

long do_wait4(int pid, int *wstatus, int options, void *rusage) {
    (void)options; (void)rusage;

    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *parent = cur->proc;

    if (wstatus && !((uint64_t)wstatus < 0x800000000000ULL &&
                     (uint64_t)wstatus + sizeof(int) <= 0x800000000000ULL &&
                     (uint64_t)wstatus + sizeof(int) >= (uint64_t)wstatus))
        return -EFAULT;

    /* Find matching child */
    for (;;) {
        int found_child = 0;

        for (int i = 0; i < PROC_MAX; i++) {
            process_t *child = &proc_pool[i];
            if (child->state == PROC_FREE) continue;
            if (child->parent_pid != parent->pid) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;

            found_child = 1;

            if (child->state == PROC_ZOMBIE) {
                int child_pid = (int)child->pid;
                int exit_status = child->exit_code;

                if (wstatus) {
                    /* Bounce via kernel variable to avoid direct user-pointer write */
                    int kstatus = (exit_status & 0xFF) << 8; /* Linux wait status format */
                    kmemcpy(wstatus, &kstatus, sizeof(kstatus));
                }

                /* Reap: free child resources */
                proc_cleanup(child);

                return child_pid;
            }
        }

        if (!found_child) return -ECHILD;

        /* Child exists but hasn't exited — spin-wait.
         * With SMP >= 2, the child runs on another core. */
        for (volatile int w = 0; w < 10000; w++)
            __asm__ volatile("pause");
    }
}
