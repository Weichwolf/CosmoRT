/* CosmoRT Process — fork, COW, address space copy */

#include "proc/proc_internal.h"

static void vma_walk(vma_t *node, void (*fn)(vma_t *, void *), void *ctx) {
    if (!node) return;
    vma_walk(node->left, fn, ctx);
    fn(node, ctx);
    vma_walk(node->right, fn, ctx);
}

static int write_pte_raw(uint64_t *user_pml4, uint64_t vaddr, uint64_t pte_val) {
    if (vaddr >= 0x800000000000ULL) return -1;
    uint64_t *pdpt = get_or_alloc_level(user_pml4, (vaddr >> 39) & 0x1FF);
    if (!pdpt) return -1;
    uint64_t *pd = get_or_alloc_level(pdpt, (vaddr >> 30) & 0x1FF);
    if (!pd) return -1;
    uint64_t *pt = get_or_alloc_level(pd, (vaddr >> 21) & 0x1FF);
    if (!pt) return -1;
    pt[(vaddr >> 12) & 0x1FF] = pte_val;
    return 0;
}

static void set_pte_inplace(uint64_t *user_pml4, uint64_t vaddr, uint64_t pte_val) {
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[(vaddr >> 39) & 0x1FF] & PTE_ADDR_MASK);
    uint64_t *pd   = (uint64_t *)phys_to_virt(pdpt[(vaddr >> 30) & 0x1FF] & PTE_ADDR_MASK);
    if (pd[(vaddr >> 21) & 0x1FF] & PTE_PS) return;
    uint64_t *pt   = (uint64_t *)phys_to_virt(pd[(vaddr >> 21) & 0x1FF] & PTE_ADDR_MASK);
    pt[(vaddr >> 12) & 0x1FF] = pte_val;
}

static int fork_split_huge_pmd(uint64_t *user_pml4, uint64_t va_base) {
    int pml4i = (va_base >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
    int pdpti = (va_base >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (va_base >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT) || !(pd[pdi] & PTE_PS)) return -1;

    uint64_t pmd = pd[pdi];
    uint64_t huge_phys = pmd & 0x000FFFFFFFE00000ULL;
    uint64_t flags = pmd & (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | PTE_COW);

    uint64_t *pt = alloc_page();
    if (!pt) return -1;

    for (int i = 0; i < 512; i++)
        pt[i] = (huge_phys + (uint64_t)i * 4096) | flags;

    pd[pdi] = virt_to_phys(pt) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return 0;
}

struct copy_ctx { uint64_t *src_pml4; uint64_t *dst_pml4; vma_t **dst_root; int err; };

static void copy_one_vma(vma_t *v, void *arg) {
    struct copy_ctx *ctx = (struct copy_ctx *)arg;
    if (ctx->err) return;

    vma_t *cv = vma_insert(ctx->dst_root, v->start, v->end, v->prot, v->flags);
    if (!cv) { ctx->err = 1; return; }
    cv->file_ino = v->file_ino;
    cv->file_offset = v->file_offset;
    cv->file_backend = v->file_backend;

    int shared = (v->flags & VMA_SHARED);

    if (!shared) {
        for (uint64_t va = v->start; va < v->end; va += 0x200000) {
            uint64_t aligned = va & 0xFFFFFFFFFFE00000ULL;
            int p4i = (aligned >> 39) & 0x1FF;
            if (!(ctx->src_pml4[p4i] & PTE_PRESENT)) continue;
            uint64_t *pdpt = (uint64_t *)phys_to_virt(ctx->src_pml4[p4i] & PTE_ADDR_MASK);
            int pi = (aligned >> 30) & 0x1FF;
            if (!(pdpt[pi] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pi] & PTE_ADDR_MASK);
            int di = (aligned >> 21) & 0x1FF;
            if ((pd[di] & PTE_PRESENT) && (pd[di] & PTE_PS))
                fork_split_huge_pmd(ctx->src_pml4, aligned);
        }
    }

    for (uint64_t va = v->start; va < v->end; va += 4096) {
        uint64_t pte = read_pte(ctx->src_pml4, va);
        if (!(pte & PTE_PRESENT)) continue;

        uint64_t src_phys = pte & PTE_ADDR_MASK;

        if (shared) {
            page_incref(src_phys);
            if (map_user_page(ctx->dst_pml4, va, src_phys, v->prot) < 0) {
                page_decref(src_phys);
                ctx->err = 1;
                return;
            }
        } else {
            uint64_t cow_pte = (pte & ~(PTE_WRITE | PTE_LAZYFREE)) | PTE_COW;
            if (pte & PTE_WRITE) {
                set_pte_inplace(ctx->src_pml4, va, cow_pte);
            } else {
                if (v->prot & PROT_WRITE)
                    cow_pte = (pte & ~(PTE_WRITE | PTE_LAZYFREE)) | PTE_COW;
                else
                    cow_pte = pte & ~PTE_LAZYFREE;
            }
            page_incref(src_phys);
            if (write_pte_raw(ctx->dst_pml4, va, cow_pte) < 0) {
                page_decref(src_phys);
                ctx->err = 1;
                return;
            }
        }
    }
}

int copy_address_space(process_t *child, process_t *parent) {
    child->pml4 = create_user_pml4();
    if (!child->pml4) return -1;

    struct copy_ctx ctx = {
        .src_pml4 = parent->pml4,
        .dst_pml4 = child->pml4,
        .dst_root = &child->vma_root,
        .err = 0
    };
    vma_walk(parent->vma_root, copy_one_vma, &ctx);

    arch_flush_tlb();
    tlb_shootdown(virt_to_phys(parent->pml4));

    return ctx.err ? -1 : 0;
}

extern void sched_add(thread_t *t);

long do_fork(void) {
    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || !cur->proc) return -EFAULT;
    process_t *parent = cur->proc;

    process_t *child = proc_alloc();
    if (!child) return -ENOMEM;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->sid  = parent->sid;
    child->vma_root = 0;

    int need_ipi = 0;
    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && (t->state == THREAD_RUNNABLE || t->state == THREAD_RUNNING)) {
            t->state = THREAD_BLOCKED;
            t->saved_priority = -2;
            if (t->cpu_affinity >= 0) need_ipi = 1;
            else need_ipi = 1;
        }
    }
    if (need_ipi) {
        extern void tlb_shootdown(uint64_t pml4_phys);
        tlb_shootdown(virt_to_phys(parent->pml4));
    }

    uint64_t fork_irqf;
    spin_lock_irq(&parent->lock, &fork_irqf);
    int copy_err = copy_address_space(child, parent);
    spin_unlock_irq(&parent->lock, fork_irqf);

    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && t->saved_priority == -2) {
            t->saved_priority = -1;
            sched_add(t);
        }
    }

    if (copy_err < 0) {
        if (child->pml4) {
            free_address_space(child->pml4);
            child->pml4 = 0;
        }
        vma_free_tree(child->vma_root);
        child->vma_root = 0;
        if (child->pid < PID_TABLE_MAX)
            pid_table[child->pid] = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }

    child->brk_base = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next = parent->mmap_next;
    child->mlockall_flags = parent->mlockall_flags;
    child->is_driver = 0;
    for (int ci = 0; ci < 256; ci++) {
        child->cwd[ci] = parent->cwd[ci];
        if (!parent->cwd[ci]) break;
    }

    child->cmdline_len = parent->cmdline_len;
    for (int ci = 0; ci < parent->cmdline_len && ci < 1024; ci++)
        child->cmdline[ci] = parent->cmdline[ci];

    child->sig_pending = 0;
    for (int si = 0; si < 64; si++)
        child->sig_actions[si] = parent->sig_actions[si];

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
                     ftype == FD_INOTIFY || ftype == FD_UNIX_SOCK)
                fd_obj_incref(ftype, parent->fds.entries[i].obj);
        }
    }
    child->fds.max_fd = parent->fds.max_fd;
    for (int w = 0; w < FD_BITMAP_WORDS; w++)
        child->fds.free_bitmap[w] = parent->fds.free_bitmap[w];

    thread_t *ct = thread_alloc();
    if (!ct) {
        free_address_space(child->pml4);
        child->pml4 = 0;
        vma_free_tree(child->vma_root);
        child->vma_root = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }

    save_user_state_for_block(cur, 0);

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
    kmemcpy(ct->fxsave_area, cur->fxsave_area, 512);
    ct->sched_policy = cur->sched_policy;
    ct->priority = cur->priority;
    ct->saved_priority = -1;
    ct->preempt_count = 0;
    ct->need_resched = 0;
    ct->cpu_affinity = -1;
    ct->timeslice = RR_TIMESLICE;
    ct->proc = child;
    ct->state = THREAD_RUNNABLE;
    ct->sig_blocked = cur->sig_blocked;

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

    extern void thread_init_kstack(thread_t *t, void (*entry)(void));
    extern void userspace_entry_trampoline(void);
    thread_init_kstack(ct, userspace_entry_trampoline);

    child->main_thread = ct;
    child->threads = ct;
    child->thread_count = 1;

    sched_add(ct);

    return (long)child->pid;
}

long do_vfork(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls) {
    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || !cur->proc) return -EFAULT;
    process_t *parent = cur->proc;

    process_t *child = proc_alloc();
    if (!child) return -ENOMEM;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->sid  = parent->sid;
    child->vma_root = 0;

    int need_ipi = 0;
    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && (t->state == THREAD_RUNNABLE || t->state == THREAD_RUNNING)) {
            t->state = THREAD_BLOCKED;
            t->saved_priority = -2;
            need_ipi = 1;
        }
    }
    if (need_ipi) {
        extern void tlb_shootdown(uint64_t pml4_phys);
        tlb_shootdown(virt_to_phys(parent->pml4));
    }

    uint64_t fork_irqf;
    spin_lock_irq(&parent->lock, &fork_irqf);
    int copy_err = copy_address_space(child, parent);
    spin_unlock_irq(&parent->lock, fork_irqf);

    for (thread_t *t = parent->threads; t; t = t->proc_next) {
        if (t != cur && t->saved_priority == -2) {
            t->saved_priority = -1;
            sched_add(t);
        }
    }

    if (copy_err < 0) {
        if (child->pml4) { free_address_space(child->pml4); child->pml4 = 0; }
        vma_free_tree(child->vma_root);
        child->vma_root = 0;
        if (child->pid < PID_TABLE_MAX) pid_table[child->pid] = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }

    child->brk_base = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next = parent->mmap_next;
    child->mlockall_flags = parent->mlockall_flags;
    child->is_driver = 0;
    for (int ci = 0; ci < 256; ci++) {
        child->cwd[ci] = parent->cwd[ci];
        if (!parent->cwd[ci]) break;
    }
    child->cmdline_len = parent->cmdline_len;
    for (int ci = 0; ci < parent->cmdline_len && ci < 1024; ci++)
        child->cmdline[ci] = parent->cmdline[ci];
    child->sig_pending = 0;
    for (int si = 0; si < 64; si++)
        child->sig_actions[si] = parent->sig_actions[si];

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
                     ftype == FD_INOTIFY || ftype == FD_UNIX_SOCK)
                fd_obj_incref(ftype, parent->fds.entries[i].obj);
        }
    }
    child->fds.max_fd = parent->fds.max_fd;
    for (int w = 0; w < FD_BITMAP_WORDS; w++)
        child->fds.free_bitmap[w] = parent->fds.free_bitmap[w];

    thread_t *ct = thread_alloc();
    if (!ct) {
        free_address_space(child->pml4); child->pml4 = 0;
        vma_free_tree(child->vma_root); child->vma_root = 0;
        if (child->pid < PID_TABLE_MAX) pid_table[child->pid] = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }

    save_user_state_for_block(cur, 0);

    ct->rip = cur->rip;
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

    ct->rsp = child_stack ? (uint64_t)child_stack : cur->rsp;

    if (flags & CLONE_SETTLS)
        ct->fs_base = tls;
    else
        ct->fs_base = cur->fs_base;

    kmemcpy(ct->fxsave_area, cur->fxsave_area, 512);
    ct->sched_policy = cur->sched_policy;
    ct->priority = cur->priority;
    ct->saved_priority = -1;
    ct->preempt_count = 0;
    ct->need_resched = 0;
    ct->cpu_affinity = -1;
    ct->timeslice = RR_TIMESLICE;
    ct->proc = child;
    ct->state = THREAD_RUNNABLE;
    ct->sig_blocked = cur->sig_blocked;

    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        if (user_ok((uint64_t)parent_tid, 4))
            *parent_tid = ct->tid;
    }

    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        if (user_ok((uint64_t)child_tid, 4))
            *child_tid = ct->tid;
    }

    ct->clear_child_tid = 0;
    ct->blocking_info = (blocking_info_t){0};
    if ((flags & CLONE_CHILD_CLEARTID) && child_tid)
        ct->clear_child_tid = child_tid;

    ct->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!ct->kstack) {
        thread_free(ct);
        free_address_space(child->pml4); child->pml4 = 0;
        vma_free_tree(child->vma_root); child->vma_root = 0;
        if (child->pid < PID_TABLE_MAX) pid_table[child->pid] = 0;
        slab_free(&proc_slab, child);
        return -ENOMEM;
    }
    ct->kstack_top = (uint64_t)(uintptr_t)(ct->kstack + KSTACK_SIZE);

    {
        extern void thread_init_kstack(thread_t *t, void (*entry)(void));
        extern void userspace_entry_trampoline(void);
        thread_init_kstack(ct, userspace_entry_trampoline);
    }

    child->main_thread = ct;
    child->threads = ct;
    child->thread_count = 1;

    child->vfork_parent_tid = cur->tid;

    cur->rax = (uint64_t)(long)child->pid;
    cur->state = THREAD_BLOCKED;

    sched_add(ct);

    extern uint64_t pml4[];
    arch_set_cr3(virt_to_phys(pml4));
    thread_return_to_kernel(cur);
    return (long)child->pid;
}
