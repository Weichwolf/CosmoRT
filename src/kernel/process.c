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

int proc_create_elf(const void *elf_data, size_t elf_len) {
    process_t *p = proc_alloc();
    if (!p) return -1;

    p->pml4 = create_user_pml4();
    if (!p->pml4) { slab_free(&proc_slab, p); return -1; }
    p->vma_root = 0;

    /* ASLR: randomize stack and mmap base */
    uint64_t stack_rand = aslr_rand() & 0xFFF000ULL;       /* 4K-aligned, ~16MB range */
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;   /* 4K-aligned, ~256GB range */
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF */
    uint64_t entry, stack_ptr, brk_end;
    if (elf_load(elf_data, elf_len, p->pml4, stack_top,
                 &entry, &stack_ptr, &brk_end) < 0) {
        slab_free(&proc_slab, p);
        return -1;
    }

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
    if (elf_len >= 64) { /* sizeof(Elf64_Ehdr) */
        const uint8_t *data = (const uint8_t *)elf_data;
        /* Quick parse: e_phoff at offset 32, e_phentsize at 54, e_phnum at 56 */
        uint64_t phoff = *(const uint64_t *)(data + 32);
        uint16_t phentsize = *(const uint16_t *)(data + 54);
        uint16_t phnum = *(const uint16_t *)(data + 56);
        for (int i = 0; i < phnum; i++) {
            const uint8_t *ph = data + phoff + (uint64_t)i * phentsize;
            uint32_t p_type = *(const uint32_t *)ph;
            if (p_type != 1) continue; /* PT_LOAD */
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
    if (!t) { slab_free(&proc_slab, p); return -1; }

    t->state = THREAD_RUNNABLE;
    t->rip = entry;
    t->rsp = stack_ptr;
    t->rflags = 0x202;
    t->sched_policy = SCHED_OTHER;
    t->priority = PRIO_MIN;
    t->saved_priority = -1; /* not boosted */
    t->cpu_affinity = -1;  /* any core */
    t->timeslice = RR_TIMESLICE;
    t->proc = p;

    /* Kernel stack for this thread */
    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) { thread_free(t); slab_free(&proc_slab, p); return -1; }
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
