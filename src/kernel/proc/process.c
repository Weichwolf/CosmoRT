/* CosmoRT Process — core init, alloc, page tables, context switching
 *
 * Slab-allocated. Each process starts with one main thread.
 * Scheduler operates on threads, not processes.
 */

#include "proc/proc_internal.h"

/* ── Slab pools ─────────────────────────────────── */

slab_t proc_slab;
slab_t thread_slab;
static int next_pid = 1;
static int next_tid = 1;
spinlock_t pid_lock = SPINLOCK_INIT;

/* O(1) lookup tables — indexed by PID/TID, entries set at alloc, cleared at free */
process_t *pid_table[PID_TABLE_MAX];
thread_t  *tid_table[TID_TABLE_MAX];

void proc_init(void) {
    slab_init_dynamic(&proc_slab, (int)sizeof(process_t), 32);
    slab_init_dynamic(&thread_slab, (int)sizeof(thread_t), 64);
    serial_puts("proc: init (dynamic slab)\n");
}

/* ── Process iteration via pid_table ─────────────── */

void proc_for_each(proc_iter_fn fn, void *ctx) {
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *p = pid_table[i];
        if (p && p->state != PROC_FREE && p->pid == (uint32_t)i) {
            if (fn(p, ctx)) return;
        }
    }
}

int proc_count_alive(void) {
    int count = 0;
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *p = pid_table[i];
        if (p && p->state == PROC_ALIVE && p->pid == (uint32_t)i)
            count++;
    }
    return count;
}

thread_t *thread_alloc(void) {
    thread_t *t = (thread_t *)slab_alloc(&thread_slab);
    if (t) {
        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        /* Find free TID slot (skip collisions from wrapping) */
        int tid = -1;
        for (int try = 0; try < TID_TABLE_MAX - 2; try++) {
            int candidate = next_tid++;
            if (next_tid >= TID_TABLE_MAX) next_tid = 2;
            if (!tid_table[candidate]) { tid = candidate; break; }
        }
        if (tid < 0) {
            spin_unlock_irq(&pid_lock, flags);
            slab_free(&thread_slab, t);
            return 0; /* TID table full */
        }
        t->tid = tid;
        tid_table[tid] = t;
        spin_unlock_irq(&pid_lock, flags);
        event_queue_init(&t->eq);
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

process_t *proc_alloc(void) {
    process_t *p = (process_t *)slab_alloc(&proc_slab);
    if (p) {
        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        /* Find free PID slot (skip collisions from wrapping) */
        int pid = -1;
        for (int try = 0; try < PID_TABLE_MAX - 2; try++) {
            int candidate = next_pid++;
            if (next_pid >= (int)PID_TABLE_MAX) next_pid = 2;
            if (!pid_table[candidate]) { pid = candidate; break; }
        }
        if (pid < 0) {
            spin_unlock_irq(&pid_lock, flags);
            slab_free(&proc_slab, p);
            return 0; /* PID table full */
        }
        p->pid = (uint32_t)pid;
        pid_table[pid] = p;
        spin_unlock_irq(&pid_lock, flags);
        p->state = PROC_ALIVE;
    }
    return p;
}

/* ── Page table management ──────────────────────── */

uint64_t *alloc_page(void) {
    return (uint64_t *)page_alloc();
}

/* Convert PROT_* flags to x86 PTE flags (leaf entry only) */
uint64_t prot_to_pte(int prot) {
    /* PROT_NONE → not present (no access at all) */
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

/* Create user PML4: lower half empty (user), upper half = kernel direct map */
uint64_t *create_user_pml4(void) {
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
uint64_t *get_or_alloc_level(uint64_t *table, int idx) {
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

    /* If PD entry is a 2MB huge page, split it into 512 × 4KB PTEs first */
    if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_PS)) {
        uint64_t pmd = pd[pd_idx];
        uint64_t huge_phys = pmd & 0x000FFFFFFFE00000ULL;
        uint64_t flags = pmd & (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | PTE_COW);
        uint64_t *pt_new = alloc_page();
        if (!pt_new) return -1;
        for (int i = 0; i < 512; i++)
            pt_new[i] = (huge_phys + (uint64_t)i * 4096) | flags;
        pd[pd_idx] = virt_to_phys(pt_new) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    uint64_t *pt = get_or_alloc_level(pd, pd_idx);
    if (!pt) return -1;

    /* Only the leaf PTE restricts access based on prot */
    pt[pt_idx] = phys | prot_to_pte(prot);
    return 0;
}

int map_user_huge_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot) {
    if (vaddr >= 0x800000000000ULL) return -1;
    if (vaddr & (0x200000ULL - 1)) return -1;  /* must be 2MB-aligned */
    if (phys  & (0x200000ULL - 1)) return -1;

    uint64_t *pdpt = get_or_alloc_level(user_pml4, (vaddr >> 39) & 0x1FF);
    if (!pdpt) return -1;
    uint64_t *pd = get_or_alloc_level(pdpt, (vaddr >> 30) & 0x1FF);
    if (!pd) return -1;

    int pd_idx = (vaddr >> 21) & 0x1FF;
    /* PMD entry with PS bit — direct 2MB mapping, no PT */
    uint64_t flags = PTE_PS | PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    pd[pd_idx] = phys | flags;
    return 0;
}

/* ── FD table init ──────────────────────────────── */

/* Default PTY for init's stdio. Set by vt_init(). */
int fd_default_pty = -1;

void fd_table_init(fd_table_t *fdt) {
    for (int i = 0; i < FD_MAX; i++) fdt->entries[i].type = FD_NONE;
    /* All FDs free */
    for (int w = 0; w < FD_BITMAP_WORDS; w++) fdt->free_bitmap[w] = ~0ULL;
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
    fd_mark_used(fdt, 0);
    fd_mark_used(fdt, 1);
    fd_mark_used(fdt, 2);
    fdt->max_fd = 3;
}

int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags) {
    int fd = fd_find_free(fdt, 0);
    if (fd < 0) return -EMFILE;
    /* Enforce RLIMIT_NOFILE */
    process_t *p = proc_current();
    if (p && p->rlim_nofile && (unsigned long)fd >= p->rlim_nofile)
        return -EMFILE;
    fdt->entries[fd] = (fd_entry_t){type, obj, flags};
    fd_mark_used(fdt, fd);
    if (fd >= fdt->max_fd) fdt->max_fd = fd + 1;
    return fd;
}

int fd_close(fd_table_t *fdt, int fd) {
    if (fd < 0 || fd >= FD_MAX) return -1;
    if (fdt->entries[fd].type == FD_NONE) return -1;
    fdt->entries[fd].type = FD_NONE;
    fdt->entries[fd].obj = 0;
    fd_mark_free(fdt, fd);
    return 0;
}

fd_entry_t *fd_get(fd_table_t *fdt, int fd) {
    if (fd < 0 || fd >= FD_MAX) return 0;
    if (fdt->entries[fd].type == FD_NONE) return 0;
    return &fdt->entries[fd];
}

/* ── ASLR ───────────────────────────────────────── */

/* ASLR via kernel CSPRNG, fallback to RDTSC before init */
uint64_t aslr_rand(void) {
    uint64_t r;
    extern int random_get(void *buf, size_t len);
    if (random_get(&r, sizeof(r)) == 0) return r;
    return arch_rdtsc();
}

/* ── Process creation ───────────────────────────── */

int proc_create_elf(const void *elf_data, size_t elf_len) {
    process_t *p = proc_alloc();
    if (!p) return -1;

    p->pml4 = create_user_pml4();
    if (!p->pml4) goto fail_slab;
    p->vma_root = 0;

    /* ASLR: randomize stack and mmap base */
    uint64_t stack_rand = aslr_rand() & 0x3FFFFF000ULL; /* 22-bit, 4KB aligned */
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF with proper argv/envp for init */
    const char *init_argv[] = { "/init" };
    const char *init_envp[] = { "HOME=/", "PATH=/bin:/usr/bin", "TERM=linux" };
    uint64_t entry, stack_ptr, brk_end;
    if (elf_load(elf_data, elf_len, p->pml4, stack_top,
                 init_argv, 1, init_envp, 3,
                 &entry, &stack_ptr, &brk_end) < 0)
        goto fail_pml4;

    p->brk_base = brk_end;
    p->brk_current = brk_end;
    p->is_driver = (p->pid == 1) ? 1 : 0;
    p->notify_signal = SIGCHLD;
    p->umask_val = 0022;
    p->pgid = p->pid;

    p->sid  = p->pid;  /* initial process is own session leader */
    p->cwd[0] = '/'; p->cwd[1] = '\0';
    { const char *s = "/init"; int ii = 0; while (s[ii] && ii < 255) { p->exe_path[ii] = s[ii]; ii++; } p->exe_path[ii] = 0; }
    { const char *s = "init";  int ii = 0; while (s[ii] && ii < 15)  { p->comm[ii] = s[ii]; ii++; } p->comm[ii] = 0; }
    { const char *s = "/init"; int ii = 0; while (s[ii] && ii < 1023) { p->cmdline[ii] = s[ii]; ii++; } p->cmdline[ii] = 0; p->cmdline_len = ii + 1; }
    fd_table_init(&p->fds);

    /* Stack: small initial VMA with VMA_GROWSDOWN (expands on page fault).
     * Like Linux: initial 132KB, grows to RLIMIT_STACK on demand. */
    uint64_t stack_bottom = stack_top - USER_STACK_INIT;
    vma_insert(&p->vma_root, stack_bottom, stack_top,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | VMA_GROWSDOWN);
    p->stack_top = stack_top;

    /* brk VMA is created on first brk() call that grows beyond brk_base.
     * Don't insert a zero-length VMA here — it corrupts the AVL tree. */

    /* Create VMAs for ELF segments by scanning the ELF headers */
    if (elf_len >= 64) {
        const uint8_t *data = (const uint8_t *)elf_data;
        uint64_t phoff = *(const uint64_t *)(data + 32);
        uint16_t phentsize = *(const uint16_t *)(data + 54);
        uint16_t phnum = *(const uint16_t *)(data + 56);
        if (phoff + (uint64_t)phnum * phentsize > elf_len)
            goto fail_elf;

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

    /* Initialize FPU/SSE/AVX state: clean XSAVE/FXSAVE image.
     * FCW=0x037F: extended precision, all x87 exceptions masked.
     * MXCSR=0x1F80: all SSE exceptions masked, round-to-nearest.
     * XSTATE_BV at offset 512 marks which components are valid. */
    *(uint16_t *)(t->xsave_area + 0) = 0x037F;   /* FCW */
    *(uint32_t *)(t->xsave_area + 24) = 0x1F80;  /* MXCSR */
    if (xsave_size > 512)
        *(uint64_t *)(t->xsave_area + 512) = xsave_xcr0; /* XSTATE_BV */

    /* Kernel stack for this thread */
    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) goto fail_thread;
    t->kstack_top = (uint64_t)(uintptr_t)(t->kstack + KSTACK_SIZE);

    p->main_thread = t;
    p->threads = t;
    p->thread_count = 1;

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

/* Create process from a VFS file path (reads file into memory, loads as ELF).
 * Used for userspace drivers like e1000d that are registered in /bin/. */
int proc_create_from_vfs(const char *path) {
    extern int vfs_read_file(const char *, uint8_t **, size_t *);
    uint8_t *data = 0;
    size_t size = 0;
    if (vfs_read_file(path, &data, &size) < 0 || !data) return -1;

    int pid = proc_create_elf(data, size);

    /* Mark as driver (gets HW access permissions) */
    if (pid > 0) {
        process_t *p = proc_find((uint32_t)pid);
        if (p) p->is_driver = 1;
    }

    int npages = (int)((size + 4095) / 4096);
    pages_free(data, npages);
    return pid;
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
        arch_set_cr3(virt_to_phys(pml4));
        cpu->current_thread = 0;
        return;
    }

    /* Load process page tables */
    arch_set_cr3(virt_to_phys(t->proc->pml4));

    /* Set kernel stack for interrupts/syscalls */
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(t->kstack_top);
    cpu->kernel_rsp = t->kstack_top;

    /* Kernel-level yield resume: thread yielded from inside a syscall
     * (e.g., net_idle in TCP connect). Kernel call stack is intact on
     * kstack. longjmp back to the kernel_yield_jmpbuf to resume.
     * Enable IRQs — kernel_longjmp doesn't restore RFLAGS. */
    if (t->in_kernel_yield) {
        t->in_kernel_yield = 0;
        arch_sti();
        extern void kernel_longjmp(uint64_t buf[8], int val) __attribute__((noreturn));
        kernel_longjmp(t->kernel_yield_jmpbuf, 1);
    }

    /* Load thread's FS base (TLS) before entering userspace — always,
     * even when fs_base == 0 (after execve, before arch_prctl SET_FS) */
    arch_set_fs_base(t->fs_base);

    /* Restore FPU/SSE/AVX state */
    arch_fpstate_restore(t->xsave_area);

    /* IRET to Ring 3 (proc_enter_ring3 reads thread_t by offsets) */
    proc_enter_ring3(t);
}

void thread_return_to_kernel(thread_t *t) {
    arch_sti();
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

/* Read PTE for a user virtual address. Returns 0 if not mapped.
 * For 2MB huge pages (PS bit in PMD): returns a synthetic PTE with
 * the correct sub-page physical address and PTE_PS set. */
uint64_t read_pte_pub(uint64_t *user_pml4, uint64_t va) {
    int pml4i = (va >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
    int pdpti = (va >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (va >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    /* Huge page: PMD entry directly maps 2MB */
    if (pd[pdi] & PTE_PS) {
        uint64_t huge_phys = pd[pdi] & 0x000FFFFFFFE00000ULL;
        uint64_t offset = va & 0x1FFFFFULL;
        uint64_t flags = pd[pdi] & ~0x000FFFFFFFE00000ULL;
        return (huge_phys + offset) | (flags & ~PTE_PS);
    }
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    int pti = (va >> 12) & 0x1FF;
    return pt[pti];
}
