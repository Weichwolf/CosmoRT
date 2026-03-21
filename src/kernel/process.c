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
#include "memops.h"
#include "syscall.h"

/* Page table flags */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PS      (1ULL << 7)

/* ── Slab pools ─────────────────────────────────── */

process_t proc_pool[PROC_MAX];
thread_t  thread_pool[THREAD_MAX];

static slab_t proc_slab;
static slab_t thread_slab;
static int next_pid = 1;
static int next_tid = 1;
static spinlock_t pid_lock = SPINLOCK_INIT;

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
        spin_unlock_irq(&pid_lock, flags);
    }
    return t;
}

void thread_free(thread_t *t) {
    if (t && t->kstack)
        pages_free(t->kstack, KSTACK_SIZE / 4096);
    slab_free(&thread_slab, t);
}

static process_t *proc_alloc(void) {
    process_t *p = (process_t *)slab_alloc(&proc_slab);
    if (p) {
        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        p->pid = (uint32_t)next_pid++;
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
        return (uint64_t *)phys_to_virt(table[idx] & ~0xFFFULL);

    uint64_t *new_tbl = alloc_page();
    if (!new_tbl) return 0;
    table[idx] = virt_to_phys(new_tbl) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return new_tbl;
}

int map_user_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys) {
    /* User pages only in lower half */
    if (vaddr >= 0x800000000000ULL) return -1;

    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    int pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_alloc_level(user_pml4, pml4_idx);
    if (!pdpt) return -1;

    uint64_t *pd = get_or_alloc_level(pdpt, pdpt_idx);
    if (!pd) return -1;

    uint64_t *pt = get_or_alloc_level(pd, pd_idx);
    if (!pt) return -1;

    pt[pt_idx] = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return 0;
}

/* ── FD table init ──────────────────────────────── */

void fd_table_init(fd_table_t *fdt) {
    for (int i = 0; i < FD_MAX; i++) fdt->entries[i].type = FD_NONE;
    /* fd 0 = stdin, fd 1 = stdout, fd 2 = stderr → serial */
    fdt->entries[0] = (fd_entry_t){FD_SERIAL, 0, O_RDONLY};
    fdt->entries[1] = (fd_entry_t){FD_SERIAL, 0, O_WRONLY};
    fdt->entries[2] = (fd_entry_t){FD_SERIAL, 0, O_WRONLY};
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

/* Simple PRNG from RDTSC for ASLR */
static uint64_t aslr_rand(void) {
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

/* ── Process lookup ──────────────────────────────── */

process_t *proc_find(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_pool[i].state != PROC_FREE && proc_pool[i].pid == pid)
            return &proc_pool[i];
    }
    return 0;
}

/* ── Address space helpers ───────────────────────── */

/* Read PTE for a user virtual address. Returns 0 if not mapped. */
static uint64_t read_pte(uint64_t *user_pml4, uint64_t va) {
    int pml4i = (va >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & ~0xFFFULL);
    int pdpti = (va >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & ~0xFFFULL);
    int pdi = (va >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & ~0xFFFULL);
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
        uint64_t src_phys = pte & ~0xFFFULL;
        void *src = phys_to_virt(src_phys);
        kmemcpy(new_page, src, 4096);

        if (map_user_page(ctx->dst_pml4, va, virt_to_phys(new_page)) < 0) {
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

    /* Walk lower half only (PML4[0..255] = user space) */
    for (int i = 0; i < 256; i++) {
        if (!(user_pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[i] & ~0xFFFULL);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & ~0xFFFULL);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & ~0xFFFULL);

                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        page_free(phys_to_virt(pt[l] & ~0xFFFULL));
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

    /* Close all FDs */
    for (int i = 0; i < FD_MAX; i++) {
        if (p->fds.entries[i].type == FD_FILE) {
            /* Free the vfs_file object */
            /* VFS file structs are slab-allocated; just free them */
            extern void vfs_file_free_obj(void *obj);
            vfs_file_free_obj(p->fds.entries[i].obj);
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
     * Only suspend RUNNABLE threads (BLOCKED threads aren't modifying memory,
     * and RUNNING threads on other cores would need IPI — acceptable for now). */
    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && t->state == THREAD_RUNNABLE) {
            t->state = THREAD_BLOCKED;
            t->saved_priority = -2; /* mark: we stopped this one */
        }
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

    /* Duplicate fd_table */
    for (int i = 0; i < FD_MAX; i++)
        child->fds.entries[i] = parent->fds.entries[i];
    child->fds.max_fd = parent->fds.max_fd;
    /* Note: VFS file objects are shared (no refcount yet — acceptable for bootstrap) */

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

    serial_puts("fork: parent=");
    serial_putchar('0' + (parent->pid % 10));
    serial_puts(" child=");
    serial_putchar('0' + (child->pid % 10));
    serial_putchar('\n');

    return (long)child->pid;
}

/* ── execve (2.2) ────────────────────────────────── */

long do_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)argv; (void)envp; /* TODO: pass argv/envp to new process */

    thread_t *cur = thread_current();
    if (!cur || !cur->proc) return -EFAULT;
    process_t *p = cur->proc;

    /* Look up the file in VFS */
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_FILE) return -EACCES;
    if (!node->data || node->size == 0) return -ENOEXEC;

    /* Copy ELF data to a kernel buffer (we're about to destroy the address space) */
    size_t elf_len = node->size;
    int elf_pages = (int)((elf_len + 4095) / 4096);
    uint8_t *elf_buf = (uint8_t *)pages_alloc(elf_pages);
    if (!elf_buf) return -ENOMEM;
    kmemcpy(elf_buf, node->data, elf_len);

    /* Free current address space */
    free_address_space(p->pml4);
    vma_free_tree(p->vma_root);
    p->vma_root = 0;

    /* Create new PML4 */
    p->pml4 = create_user_pml4();
    if (!p->pml4) {
        pages_free(elf_buf, elf_pages);
        /* Process is now dead — no address space */
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(cur);
        return -ENOMEM; /* unreachable */
    }

    /* ASLR */
    uint64_t stack_rand = aslr_rand() & 0xFFF000ULL;
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF */
    uint64_t entry, stack_ptr, brk_end;
    if (elf_load(elf_buf, elf_len, p->pml4, stack_top,
                 &entry, &stack_ptr, &brk_end) < 0) {
        pages_free(elf_buf, elf_pages);
        p->state = PROC_ZOMBIE;
        cur->state = THREAD_DEAD;
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(cur);
        return -ENOEXEC; /* unreachable */
    }
    pages_free(elf_buf, elf_pages);

    p->brk_base = brk_end;
    p->brk_current = brk_end;

    /* Stack VMA */
    uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
    vma_insert(&p->vma_root, stack_bottom, stack_top,
               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (brk_end > 0)
        vma_insert(&p->vma_root, brk_end, brk_end,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

    /* ELF segment VMAs */
    /* Re-parse from the loaded pages — entry already validated in elf_load */

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

    if (wstatus && !((uint64_t)wstatus < 0x800000000000ULL))
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

                if (wstatus)
                    *wstatus = (exit_status & 0xFF) << 8; /* Linux wait status format */

                /* Reap: free child resources */
                proc_cleanup(child);

                return child_pid;
            }
        }

        if (!found_child) return -ECHILD;

        /* Child exists but hasn't exited — block */
        save_user_state_for_block(cur, 0);
        cur->state = THREAD_BLOCKED;
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(cur);
        /* When woken, loop again to check for zombie */
    }
}
