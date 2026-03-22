/* CosmoRT Syscall Layer — signal handling */

#include "internal.h"

/* ── Signal delivery (full: SIG_DFL + SIG_IGN + user handlers) ──── */

/* Resolve user virtual address to kernel-accessible pointer via page table walk.
 * Returns kernel virtual pointer or NULL if page not mapped. */
static void *resolve_user_addr(uint64_t *user_pml4, uint64_t uaddr) {
    int pml4i = (uaddr >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
    int pdpti = (uaddr >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (uaddr >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    int pti = (uaddr >> 12) & 0x1FF;
    if (!(pt[pti] & PTE_PRESENT)) return 0;
    uint64_t phys_page = pt[pti] & PTE_ADDR_MASK;
    return (void *)((uint64_t)phys_to_virt(phys_page) + (uaddr & 0xFFF));
}

/* Ensure user page is mapped (demand-page if needed). Returns 0 on success. */
static int ensure_user_page(process_t *p, uint64_t uaddr) {
    if (resolve_user_addr(p->pml4, uaddr)) return 0;
    /* Page not mapped — allocate and map with RW */
    uint64_t page_addr = uaddr & ~0xFFFULL;
    uint64_t *page = alloc_page();
    if (!page) return -ENOMEM;
    kmemset(page, 0, 4096);
    return map_user_page(p->pml4, page_addr, virt_to_phys(page), 0x3 /* PROT_READ|PROT_WRITE */);
}

/* No SMAP in CosmoRT — user memory is directly accessible from kernel mode.
 * These helpers validate the address range before access. */

/* ── Signal frame layout (Linux-compatible) ──────────────── */

/* mcontext_t register layout (Linux x86_64 compatible) */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, rflags;
    uint64_t cs, gs, fs, err, trapno, oldmask, cr2; /* zeroed */
} sig_mcontext_t; /* 25 * 8 = 200 bytes */

typedef struct {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp, ss_flags, ss_size; /* uc_stack */
    sig_mcontext_t uc_mcontext;
    uint64_t uc_sigmask;
} sig_ucontext_t; /* 8 + 8 + 24 + 200 + 8 = 248 bytes */

typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
    uint8_t _pad[128 - 16]; /* pad to 128 bytes */
} sig_siginfo_t;

/* On-stack trampoline: mov rax, 15; syscall (8 bytes) */
static const uint8_t sig_trampoline[] = {
    0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00, /* mov rax, 15 */
    0x0f, 0x05                                   /* syscall */
};

/* Total signal frame: restorer_addr(8) + siginfo(128) + ucontext(248) + trampoline(9) */
#define SIG_FRAME_SIZE  (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t) + sizeof(sig_trampoline))

/* Offsets from new RSP (bottom of frame):
 *   [RSP+0]   = return address (restorer or trampoline)
 *   [RSP+8]   = siginfo_t
 *   [RSP+136] = ucontext_t
 *   [RSP+384] = trampoline bytes (if no sa_restorer)
 */
#define SIGFRAME_OFF_RETADDR   0
#define SIGFRAME_OFF_SIGINFO   8
#define SIGFRAME_OFF_UCONTEXT  (8 + sizeof(sig_siginfo_t))
#define SIGFRAME_OFF_TRAMPOLINE (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t))

/* Deliver signal to user thread by pushing signal frame onto user stack.
 * Modifies thread registers so next return-to-userspace enters the handler.
 * Called from check_pending_signals (SYSCALL return or timer preempt path). */
void deliver_signal(thread_t *t, int signo) {
    process_t *p = t->proc;
    struct k_sigaction *sa = &p->sig_actions[signo];

    if ((uint64_t)sa->sa_handler <= 1) return; /* SIG_DFL or SIG_IGN — shouldn't be here */

    /* Compute frame location on user stack */
    uint64_t frame_size = SIG_FRAME_SIZE;
    /* Add trampoline only if no sa_restorer */
    int has_restorer = (sa->sa_flags & SA_RESTORER) && sa->sa_restorer;
    if (has_restorer)
        frame_size -= sizeof(sig_trampoline);
    uint64_t new_rsp = (t->rsp - frame_size) & ~0xFULL; /* 16-byte align */

    /* Verify target stack area is in a writable VMA */
    vma_t *vma = vma_find(p->vma_root, new_rsp);
    if (!vma || new_rsp < vma->start || (new_rsp + frame_size) > vma->end
        || !(vma->prot & PROT_WRITE)) {
        do_exit(128 + signo);
        return;
    }

    /* Ensure all pages in the frame are mapped */
    for (uint64_t addr = new_rsp & ~0xFFFULL; addr < new_rsp + frame_size; addr += 4096) {
        if (ensure_user_page(p, addr) < 0) {
            /* Can't allocate stack page — kill process */
            do_exit(128 + signo);
            return;
        }
    }

    /* Build ucontext (save current registers) */
    sig_ucontext_t uc;
    kmemset(&uc, 0, sizeof(uc));
    uc.uc_mcontext.r8  = t->r8;  uc.uc_mcontext.r9  = t->r9;
    uc.uc_mcontext.r10 = t->r10; uc.uc_mcontext.r11 = t->r11;
    uc.uc_mcontext.r12 = t->r12; uc.uc_mcontext.r13 = t->r13;
    uc.uc_mcontext.r14 = t->r14; uc.uc_mcontext.r15 = t->r15;
    uc.uc_mcontext.rdi = t->rdi; uc.uc_mcontext.rsi = t->rsi;
    uc.uc_mcontext.rbp = t->rbp; uc.uc_mcontext.rbx = t->rbx;
    uc.uc_mcontext.rdx = t->rdx; uc.uc_mcontext.rax = t->rax;
    uc.uc_mcontext.rcx = t->rcx; uc.uc_mcontext.rsp = t->rsp;
    uc.uc_mcontext.rip = t->rip; uc.uc_mcontext.rflags = t->rflags;
    uc.uc_sigmask = p->sig_blocked;

    /* Build siginfo */
    sig_siginfo_t si;
    kmemset(&si, 0, sizeof(si));
    si.si_signo = (int32_t)signo;
    if (signo == 11 || signo == 7) { /* SIGSEGV / SIGBUS */
        si.si_code = 1; /* SEGV_MAPERR */
        /* si_addr at offset 16 in siginfo_t (after signo, errno, code, pad) */
        uint64_t fault = t->fault_addr;
        kmemcpy(&si._pad[0], &fault, 8);
    } else {
        si.si_code = 0; /* SI_USER */
    }

    /* Write return address */
    uint64_t restorer_addr;
    if (has_restorer) {
        restorer_addr = (uint64_t)sa->sa_restorer;
    } else {
        restorer_addr = new_rsp + SIGFRAME_OFF_TRAMPOLINE;
    }

    /* Write signal frame directly to user stack.
     * No SMAP in CosmoRT, CR3 = user page tables during SYSCALL. */
    *(uint64_t *)new_rsp = restorer_addr;
    kmemcpy((void *)(new_rsp + SIGFRAME_OFF_SIGINFO), &si, sizeof(si));
    kmemcpy((void *)(new_rsp + SIGFRAME_OFF_UCONTEXT), &uc, sizeof(uc));

    /* Write trampoline if no sa_restorer */
    if (!has_restorer)
        kmemcpy((void *)(new_rsp + SIGFRAME_OFF_TRAMPOLINE), sig_trampoline, sizeof(sig_trampoline));

    /* Set up thread to enter handler */
    t->rip = (uint64_t)sa->sa_handler;
    t->rsp = new_rsp;
    t->rdi = (uint64_t)signo;
    t->rsi = new_rsp + SIGFRAME_OFF_SIGINFO;
    t->rdx = new_rsp + SIGFRAME_OFF_UCONTEXT;
    /* Clear direction flag, keep interrupts enabled */
    t->rflags &= ~(1ULL << 10); /* DF=0 */
    t->rflags |= (1ULL << 9);   /* IF=1 */

    /* Block this signal during handler + sa_mask */
    p->sig_blocked |= (1ULL << signo) | sa->sa_mask;
    /* SIGKILL/SIGSTOP never blocked */
    p->sig_blocked &= ~((1ULL << 9) | (1ULL << 19));
}

/* Check and deliver pending signals. Operates on thread_t register fields.
 * Callers must sync hardware frame ↔ thread_t before/after. */
void check_pending_signals(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;

    uint64_t deliverable = p->sig_pending & ~p->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < 32; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;
        p->sig_pending &= ~(1ULL << sig);

        struct k_sigaction *sa = &p->sig_actions[sig];
        uint64_t handler = (uint64_t)sa->sa_handler;

        if (handler == 1) continue; /* SIG_IGN */

        if (handler == 0) {
            /* SIG_DFL */
            /* SIGCHLD (17): default is ignore */
            if (sig == 17) continue;
            /* Fatal signals: SIGKILL=9, SIGSEGV=11, SIGPIPE=13, SIGTERM=15, SIGABRT=6 */
            if (sig == 9 || sig == 11 || sig == 13 || sig == 15 || sig == 6) {
                do_exit(128 + sig); /* doesn't return */
            }
            /* Others: default ignore */
            continue;
        }

        /* User handler — deliver via thread_t modification */
        deliver_signal(t, sig);
        return; /* deliver one signal at a time */
    }
}

/* ── Signals (2.4) ───────────────────────────────── */

#define SIG_DFL  ((void *)0)
#define SIG_IGN  ((void *)1)

/* SIGKILL=9, SIGSEGV=11, SIGPIPE=13, SIGCHLD=17, SIGTERM=15 */

long do_rt_sigaction(int sig, const void *act_,
                            void *oldact_, size_t sigsetsize) {
    const struct k_sigaction *act = (const struct k_sigaction *)act_;
    struct k_sigaction *oldact = (struct k_sigaction *)oldact_;
    (void)sigsetsize;
    if (sig < 1 || sig >= 32) return -EINVAL;
    if (sig == 9) return -EINVAL; /* SIGKILL cannot be caught */

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    if (oldact) {
        if (!user_ok((uint64_t)oldact, sizeof(struct k_sigaction))) return -EFAULT;
        *oldact = p->sig_actions[sig];
    }

    if (act) {
        if (!user_ok((uint64_t)act, sizeof(struct k_sigaction))) return -EFAULT;
        struct k_sigaction k_act;
        kmemcpy(&k_act, act, sizeof(k_act));
        p->sig_actions[sig] = k_act;
    }

    return 0;
}

long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                              size_t sigsetsize) {
    (void)sigsetsize;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    if (oldset) {
        if (!user_ok((uint64_t)oldset, 8)) return -EFAULT;
        *oldset = p->sig_blocked;
    }

    if (set) {
        if (!user_ok((uint64_t)set, 8)) return -EFAULT;
        uint64_t k_set;
        kmemcpy(&k_set, set, 8);
        uint64_t mask = k_set;
        mask &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL, SIGSTOP cannot be blocked */
        switch (how) {
        case 0: p->sig_blocked |= mask; break;  /* SIG_BLOCK */
        case 1: p->sig_blocked &= ~mask; break; /* SIG_UNBLOCK */
        case 2: p->sig_blocked = mask; break;    /* SIG_SETMASK */
        default: return -EINVAL;
        }
    }

    return 0;
}

long do_kill(int pid, int sig) {
    if (sig < 0 || sig >= 32) return -EINVAL;
    if (sig == 0) return 0; /* check permission only */

    process_t *target = 0;
    if (pid > 0) {
        target = proc_find((uint32_t)pid);
    } else if (pid == 0 || pid == -1) {
        /* Signal to self or all — just handle self */
        target = proc_current();
    }
    if (!target) return -ESRCH;

    /* Check handler */
    void *handler = target->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    /* SIG_DFL: kill the process for fatal signals */
    if (handler == SIG_DFL) {
        /* SIGCHLD default = ignore */
        if (sig == 17) return 0; /* SIGCHLD */

        /* Fatal signals: terminate entire process group */
        if (sig == 6 || sig == 9 || sig == 11 || sig == 13 || sig == 15) {
            if (target == proc_current()) {
                do_exit_group(128 + sig); /* doesn't return */
            }
            /* Remote kill: mark zombie + kill threads */
            target->state = PROC_ZOMBIE;
            target->exit_code = 128 + sig;
            thread_t *t = target->threads;
            while (t) {
                if (t->state == THREAD_BLOCKED || t->state == THREAD_RUNNING)
                    t->state = THREAD_DEAD;
                t = t->proc_next;
            }
        }
        return 0;
    }

    /* User handler registered — set pending bit.
     * Delivery happens on return to userspace via check_pending_signals. */
    target->sig_pending |= (1ULL << sig);
    return 0;
}

/* ── SYS_RT_SIGRETURN (15) ──────────────────────────── */

long do_rt_sigreturn(void) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;
    if (!t || !t->proc) return -EFAULT;
    process_t *p = t->proc;

    /* After the handler did `ret`, RSP points past the return address.
     * The restorer then called `syscall` for SYS_RT_SIGRETURN.
     * The user RSP at syscall entry = restorer's RSP.
     * But the restorer is a simple `mov rax,15; syscall` — no stack ops.
     * So user RSP = frame base + 8 (return addr was popped by handler's `ret`).
     * We need to find the signal frame at user_rsp - 8. */
    uint64_t frame_rsp = cpu->user_rsp - 8;

    /* Read ucontext from the signal frame (direct access — no SMAP) */
    uint64_t uc_addr = frame_rsp + SIGFRAME_OFF_UCONTEXT;
    if (!user_ok(uc_addr, sizeof(sig_ucontext_t))) return -EFAULT;
    sig_ucontext_t uc;
    kmemcpy(&uc, (const void *)uc_addr, sizeof(uc));

    /* Restore registers from ucontext into syscall frame.
     * The SYSRET epilog in syscall_entry.asm will pop these. */
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    frame->r15 = uc.uc_mcontext.r15; frame->r14 = uc.uc_mcontext.r14;
    frame->r13 = uc.uc_mcontext.r13; frame->r12 = uc.uc_mcontext.r12;
    frame->rbp = uc.uc_mcontext.rbp; frame->rbx = uc.uc_mcontext.rbx;
    frame->r9  = uc.uc_mcontext.r9;  frame->r8  = uc.uc_mcontext.r8;
    frame->r10 = uc.uc_mcontext.r10; frame->rdx = uc.uc_mcontext.rdx;
    frame->rsi = uc.uc_mcontext.rsi; frame->rdi = uc.uc_mcontext.rdi;
    frame->rax = uc.uc_mcontext.rax;
    frame->r11 = uc.uc_mcontext.rflags; /* SYSRET restores RFLAGS from R11 */
    frame->rcx = uc.uc_mcontext.rip;    /* SYSRET restores RIP from RCX */
    cpu->user_rsp = uc.uc_mcontext.rsp;

    /* Restore signal mask */
    p->sig_blocked = uc.uc_sigmask;
    p->sig_blocked &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL/SIGSTOP never blocked */

    /* Return value doesn't matter — RAX is restored from ucontext.
     * But the syscall_entry.asm overwrites RAX with our return value AFTER
     * we set frame->rax. We need RAX to be the saved value.
     * Trick: return the saved RAX so it ends up correct. */
    return (long)uc.uc_mcontext.rax;
}

/* Check and deliver signals in the SYSCALL return path.
 * Syncs percpu syscall frame <-> thread_t around delivery.
 * Called from syscall_entry.asm between sys_handler return and SYSRET.
 * Actually called from the ASM-adjacent C code. */
void check_signals_syscall_path(long *result_ptr, long num) {
    if (num == SYS_RT_SIGRETURN) return;
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;
    uint64_t deliverable = p->sig_pending & ~p->sig_blocked;
    if (!deliverable) return;

    percpu_t *cpu = percpu_self();
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    /* Save syscall frame -> thread_t */
    t->rip    = frame->rcx;
    t->rflags = frame->r11;
    t->rsp    = cpu->user_rsp;
    t->rax    = (uint64_t)*result_ptr;
    t->rbx = frame->rbx; t->rdx = frame->rdx;
    t->rsi = frame->rsi; t->rdi = frame->rdi; t->rbp = frame->rbp;
    t->r8  = frame->r8;  t->r9  = frame->r9;  t->r10 = frame->r10;
    t->r12 = frame->r12; t->r13 = frame->r13;
    t->r14 = frame->r14; t->r15 = frame->r15;

    check_pending_signals();

    /* Write back thread_t -> syscall frame */
    frame->rcx = t->rip;
    frame->r11 = t->rflags;
    cpu->user_rsp = t->rsp;
    *result_ptr = (long)t->rax;
    frame->rbx = t->rbx; frame->rdx = t->rdx;
    frame->rsi = t->rsi; frame->rdi = t->rdi; frame->rbp = t->rbp;
    frame->r8  = t->r8;  frame->r9  = t->r9;  frame->r10 = t->r10;
    frame->r12 = t->r12; frame->r13 = t->r13;
    frame->r14 = t->r14; frame->r15 = t->r15;
}
