/* CosmoRT Syscall Layer — signal handling */

#include "internal.h"

/* sigaltstack flags */
#define SS_ONSTACK 1
#define SS_DISABLE 2

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

/* ── Signal frame layout (Linux x86_64 ABI-compatible) ───── */

/* Linux x86_64 gregset_t: 23 × uint64_t = 184 bytes
 * Index: R8=0 R9=1 R10=2 R11=3 R12=4 R13=5 R14=6 R15=7
 *        RDI=8 RSI=9 RBP=10 RBX=11 RDX=12 RAX=13 RCX=14 RSP=15
 *        RIP=16 EFL=17 CSGSFS=18 ERR=19 TRAPNO=20 OLDMASK=21 CR2=22 */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;  /* 0-56   */
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp; /* 64-120 */
    uint64_t rip, eflags;                              /* 128-136 */
    uint64_t csgsfs;       /* cs|gs|fs packed: cs(16)|gs(16)|fs(16)|pad(16) */
    uint64_t err, trapno, oldmask, cr2;                /* 152-176 */
} sig_gregset_t; /* 23 * 8 = 184 bytes */

/* Linux x86_64 _fpstate (FXSAVE layout) — 512 bytes.
 * No aligned attribute here — alignment for FXSAVE/FXRSTOR is handled
 * via separate temp buffers. Struct packing must match Linux ABI exactly. */
typedef struct {
    uint16_t cwd, swd, twd, fop;        /* 0-6   */
    uint64_t rip, rdp;                   /* 8-24  */
    uint32_t mxcsr, mxcsr_mask;          /* 24-32 */
    uint8_t  st_space[128];              /* 32-160: 8 × 16-byte FP regs */
    uint8_t  xmm_space[256];             /* 160-416: 16 × 16-byte XMM regs */
    uint8_t  _reserved[96];              /* 416-512 */
} sig_fpstate_t; /* 512 bytes */

/* Linux x86_64 mcontext_t: gregset(184) + fpstate_ptr(8) + reserved(64) = 256 bytes */
typedef struct {
    sig_gregset_t gregs;     /* offset 0:   184 bytes */
    uint64_t      fpstate;   /* offset 184: pointer to sig_fpstate_t */
    uint64_t      _reserved[8]; /* offset 192: 64 bytes padding */
} sig_mcontext_t; /* 256 bytes */

/* Linux x86_64 stack_t: 24 bytes */
typedef struct {
    uint64_t ss_sp;          /* 0  */
    int32_t  ss_flags;       /* 8  */
    int32_t  _pad;           /* 12 */
    uint64_t ss_size;        /* 16 */
} sig_stack_t; /* 24 bytes */

/* Linux x86_64 ucontext_t: 936 bytes total
 * uc_flags(8) + uc_link(8) + uc_stack(24) + uc_mcontext(256) +
 * uc_sigmask(128) + __fpregs_mem(512) = 936 */
typedef struct {
    uint64_t       uc_flags;        /* offset 0   */
    uint64_t       uc_link;         /* offset 8   */
    sig_stack_t    uc_stack;        /* offset 16  (24 bytes) */
    sig_mcontext_t uc_mcontext;     /* offset 40  (256 bytes) */
    uint64_t       uc_sigmask[16];  /* offset 296 (128 bytes, _NSIG/8) */
    sig_fpstate_t  __fpregs_mem;    /* offset 424 (512 bytes, inline FXSAVE area) */
} sig_ucontext_t; /* 936 bytes */

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

/* Total signal frame: restorer_addr(8) + siginfo(128) + ucontext(936) + trampoline(9) */
#define SIG_FRAME_SIZE  (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t) + sizeof(sig_trampoline))

/* Offsets from new RSP (bottom of frame):
 *   [RSP+0]    = return address (restorer or trampoline)
 *   [RSP+8]    = siginfo_t  (128 bytes)
 *   [RSP+136]  = ucontext_t (936 bytes)
 *   [RSP+1072] = trampoline bytes (if no sa_restorer)
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

    /* Compute frame location on user stack (or alternate signal stack) */
    uint64_t frame_size = SIG_FRAME_SIZE;
    /* Add trampoline only if no sa_restorer */
    int has_restorer = (sa->sa_flags & SA_RESTORER) && sa->sa_restorer;
    if (has_restorer)
        frame_size -= sizeof(sig_trampoline);

    uint64_t stack_rsp = t->rsp;
    /* Use alternate signal stack if SA_ONSTACK and altstack is configured */
    if ((sa->sa_flags & SA_ONSTACK) && t->sigalt_sp &&
        !(t->sigalt_flags & SS_DISABLE)) {
        /* Only switch if not already on the altstack */
        if (t->rsp < t->sigalt_sp || t->rsp >= t->sigalt_sp + t->sigalt_size)
            stack_rsp = t->sigalt_sp + t->sigalt_size;
    }
    stack_rsp -= 128; /* Skip x86_64 red zone (ABI mandates 128 bytes below RSP) */
    /* Align RSP for signal handler entry: handler sees RSP with a fake
     * return address at [RSP], so RSP ≡ 8 (mod 16) — same as after a call.
     * Linux: round_down(sp - frame_size, 16) - 8 */
    uint64_t new_rsp = ((stack_rsp - frame_size) & ~0xFULL) - 8;

    /* Verify target stack area is in a writable VMA */
    vma_t *vma = vma_find(p->vma_root, new_rsp);
    if (!vma || new_rsp < vma->start || (new_rsp + frame_size) > vma->end
        || !(vma->prot & PROT_WRITE)) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

    /* Ensure all pages in the frame are mapped */
    for (uint64_t addr = new_rsp & ~0xFFFULL; addr < new_rsp + frame_size; addr += 4096) {
        if (ensure_user_page(p, addr) < 0) {
            /* Can't allocate stack page — kill process */
            p->exit_signal = signo;
            do_exit(128 + signo);
            return;
        }
    }

    /* Build ucontext (save current registers — Linux x86_64 ABI) */
    sig_ucontext_t uc;
    kmemset(&uc, 0, sizeof(uc));

    /* gregset: 23 × uint64_t in Linux order */
    uc.uc_mcontext.gregs.r8  = t->r8;  uc.uc_mcontext.gregs.r9  = t->r9;
    uc.uc_mcontext.gregs.r10 = t->r10; uc.uc_mcontext.gregs.r11 = t->r11;
    uc.uc_mcontext.gregs.r12 = t->r12; uc.uc_mcontext.gregs.r13 = t->r13;
    uc.uc_mcontext.gregs.r14 = t->r14; uc.uc_mcontext.gregs.r15 = t->r15;
    uc.uc_mcontext.gregs.rdi = t->rdi; uc.uc_mcontext.gregs.rsi = t->rsi;
    uc.uc_mcontext.gregs.rbp = t->rbp; uc.uc_mcontext.gregs.rbx = t->rbx;
    uc.uc_mcontext.gregs.rdx = t->rdx; uc.uc_mcontext.gregs.rax = t->rax;
    uc.uc_mcontext.gregs.rcx = t->rcx; uc.uc_mcontext.gregs.rsp = t->rsp;
    uc.uc_mcontext.gregs.rip = t->rip; uc.uc_mcontext.gregs.eflags = t->rflags;
    uc.uc_mcontext.gregs.csgsfs = 0x33 | ((uint64_t)0 << 16) | ((uint64_t)0 << 32);

    /* Save FS_BASE in _reserved[0] for restoration in rt_sigreturn */
    uc.uc_mcontext._reserved[0] = t->fs_base;

    /* fpstate pointer → inline __fpregs_mem (patched below after frame address known) */
    /* uc_mcontext.fpstate will be set to point to __fpregs_mem on user stack */

    /* uc_sigmask: 128 bytes, store blocked mask in first word */
    uc.uc_sigmask[0] = t->sig_blocked;

    /* Save FPU/SSE state into inline __fpregs_mem.
     * FXSAVE requires 16-byte aligned operand, so use a temp buffer. */
    {
        sig_fpstate_t _fxbuf __attribute__((aligned(16)));
        __asm__ volatile("fxsave %0" : "=m"(_fxbuf));
        kmemcpy(&uc.__fpregs_mem, &_fxbuf, sizeof(sig_fpstate_t));
    }

    /* Build siginfo */
    sig_siginfo_t si;
    kmemset(&si, 0, sizeof(si));
    si.si_signo = (int32_t)signo;
    if (signo == SIGSEGV || signo == SIGBUS) {
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

    /* Set fpstate pointer to the inline __fpregs_mem on user stack.
     * __fpregs_mem is at offset 424 within ucontext_t. */
    uint64_t uc_user_addr = new_rsp + SIGFRAME_OFF_UCONTEXT;
    uc.uc_mcontext.fpstate = uc_user_addr + __builtin_offsetof(sig_ucontext_t, __fpregs_mem);

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
    if (sa->sa_flags & SA_SIGINFO) {
        /* SA_SIGINFO: handler(int sig, siginfo_t *info, void *ucontext) */
        t->rsi = new_rsp + SIGFRAME_OFF_SIGINFO;
        t->rdx = new_rsp + SIGFRAME_OFF_UCONTEXT;
    } else {
        /* Classic handler(int sig) — rsi/rdx undefined, zero for safety */
        t->rsi = 0;
        t->rdx = 0;
    }
    /* Clear RCX/R11 — stale kernel values must not leak to handler.
     * Linux clears these; SYSRET would clobber them anyway. */
    t->rcx = 0;
    t->r11 = 0;
    /* Clear direction flag, keep interrupts enabled */
    t->rflags &= ~(1ULL << 10); /* DF=0 */
    t->rflags |= (1ULL << 9);   /* IF=1 */

    /* Block this signal during handler (unless SA_NODEFER) + sa_mask */
    if (!(sa->sa_flags & SA_NODEFER))
        t->sig_blocked |= (1ULL << signo);
    t->sig_blocked |= sa->sa_mask;
    /* SIGKILL/SIGSTOP never blocked */
    t->sig_blocked &= ~((1ULL << 9) | (1ULL << 19));

    /* SA_RESETHAND: one-shot handler, reset to SIG_DFL after delivery */
    if (sa->sa_flags & SA_RESETHAND)
        sa->sa_handler = SIG_DFL;
}

/* Check and deliver pending signals. Operates on thread_t register fields.
 * Callers must sync hardware frame ↔ thread_t before/after. */
void check_pending_signals(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;

    /* If returning from rt_sigsuspend, restore the original mask before
     * delivering the signal so the ucontext captures the pre-sigsuspend mask. */
    if (t->in_sigsuspend) {
        t->sig_blocked = t->sig_saved_mask;
        t->in_sigsuspend = 0;
    }

    uint64_t deliverable = p->sig_pending & ~t->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < 64; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;
        p->sig_pending &= ~(1ULL << sig);

        struct k_sigaction *sa = &p->sig_actions[sig];
        uint64_t handler = (uint64_t)sa->sa_handler;

        if (handler == 1) continue; /* SIG_IGN */

        if (handler == 0) {
            /* SIG_DFL — POSIX default actions */
            switch (sig) {
            /* Ignore */
            case 17: /* SIGCHLD */
            case 23: /* SIGURG */
            case 28: /* SIGWINCH */
            case 29: /* SIGIO */
                continue;
            /* Stop (SIGTSTP, SIGTTIN, SIGTTOU) — ignore for now (no job control) */
            case 20: case 21: case 22:
                continue;
            /* Continue (SIGCONT) — ignore for now (no job control) */
            case 18:
                continue;
            /* Terminate: everything else with SIG_DFL */
            default:
                p->exit_signal = sig;
                do_exit(128 + sig); /* doesn't return */
            }
        }

        /* User handler — deliver via thread_t modification */
        deliver_signal(t, sig);
        return; /* deliver one signal at a time */
    }
}

/* ── SYS_SIGALTSTACK (131) ───────────────────────── */

struct k_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
};

long do_sigaltstack(const void *ss_, void *oss_) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    /* Return old stack first */
    if (oss_) {
        struct k_stack_t koss;
        koss.ss_sp    = t->sigalt_sp;
        koss.ss_size  = t->sigalt_size;
        koss._pad     = 0;
        /* SS_ONSTACK if currently executing on the altstack */
        if (t->sigalt_sp && !(t->sigalt_flags & SS_DISABLE) &&
            t->rsp >= t->sigalt_sp && t->rsp < t->sigalt_sp + t->sigalt_size)
            koss.ss_flags = SS_ONSTACK;
        else if (t->sigalt_flags & SS_DISABLE)
            koss.ss_flags = SS_DISABLE;
        else
            koss.ss_flags = 0;
        int r = copy_to_user(oss_, &koss, sizeof(koss));
        if (r) return r;
    }

    if (ss_) {
        struct k_stack_t kss;
        int r = copy_from_user(&kss, ss_, sizeof(kss));
        if (r) return r;

        if (kss.ss_flags & ~SS_DISABLE) return -EINVAL; /* unknown flags */

        if (kss.ss_flags & SS_DISABLE) {
            t->sigalt_sp    = 0;
            t->sigalt_size  = 0;
            t->sigalt_flags = SS_DISABLE;
        } else {
            if (kss.ss_size < 2048) return -ENOMEM; /* MINSIGSTKSZ */
            t->sigalt_sp    = kss.ss_sp;
            t->sigalt_size  = kss.ss_size;
            t->sigalt_flags = 0;
        }
    }

    return 0;
}

/* ── Signals (2.4) ───────────────────────────────── */

/* SIG_DFL, SIG_IGN, SIGKILL, SIGSEGV etc. — from linux.h */

long do_rt_sigaction(int sig, const void *act_,
                            void *oldact_, size_t sigsetsize) {
    const struct k_sigaction *act = (const struct k_sigaction *)act_;
    struct k_sigaction *oldact = (struct k_sigaction *)oldact_;
    /* musl passes sigsetsize=8 (64 signals). Accept 8 or 16. */
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    if (sig < 1 || sig >= 64) return -EINVAL;
    if (sig == SIGKILL) return -EINVAL; /* SIGKILL cannot be caught */

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    if (oldact) {
        int r = copy_to_user(oldact, &p->sig_actions[sig], sizeof(struct k_sigaction));
        if (r) return r;
    }

    if (act) {
        struct k_sigaction k_act;
        int r = copy_from_user(&k_act, act, sizeof(k_act));
        if (r) return r;
        p->sig_actions[sig] = k_act;
    }

    return 0;
}

long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                              size_t sigsetsize) {
    /* musl passes _NSIG/8 = 128/8 = 16 bytes (128 signals).
     * We support 64 signals (8 bytes). Accept 8 or 16, ignore upper bytes. */
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    if (oldset) {
        uint64_t tmp = t->sig_blocked;
        int r = copy_to_user(oldset, &tmp, 8);
        if (r) return r;
        /* Zero upper bytes if sigsetsize == 16 */
        if (sigsetsize == 16) {
            uint64_t zero = 0;
            r = copy_to_user(oldset + 1, &zero, 8);
            if (r) return r;
        }
    }

    if (set) {
        uint64_t k_set;
        int r = copy_from_user(&k_set, set, 8); /* only first 8 bytes matter */
        if (r) return r;
        uint64_t mask = k_set;
        mask &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL, SIGSTOP cannot be blocked */
        switch (how) {
        case 0: t->sig_blocked |= mask; break;  /* SIG_BLOCK */
        case 1: t->sig_blocked &= ~mask; break; /* SIG_UNBLOCK */
        case 2: t->sig_blocked = mask; break;    /* SIG_SETMASK */
        default: return -EINVAL;
        }
    }

    return 0;
}

/* Send signal to a single process. Returns 0 or negative errno. */
static long kill_one(process_t *target, int sig) {
    /* Check handler */
    void *handler = target->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    /* SIG_DFL: kill the process for fatal signals */
    if (handler == SIG_DFL) {
        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, SIGIO */
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) return 0;
        /* Stop: SIGTSTP, SIGTTIN, SIGTTOU — ignore for now */
        if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) return 0;
        /* Continue: SIGCONT — ignore for now */
        if (sig == SIGCONT) return 0;

        /* Everything else: terminate */
        target->exit_signal = sig;
        if (target == proc_current()) {
            do_exit_group(128 + sig); /* doesn't return */
        }
        /* Remote kill: mark zombie + kill threads */
        target->state = PROC_ZOMBIE;
        target->exit_code = 128 + sig;
        {
            thread_t *t = target->threads;
            while (t) {
                if (t->state == THREAD_BLOCKED || t->state == THREAD_RUNNING)
                    t->state = THREAD_DEAD;
                t = t->proc_next;
            }
        }
        /* Wake parent if blocked in wait4 */
        if (target->parent_pid) {
            process_t *parent = proc_find(target->parent_pid);
            if (parent) {
                extern void sched_add(thread_t *t);
                uint64_t pflags;
                spin_lock_irq(&parent->lock, &pflags);
                thread_t *pt = parent->threads;
                while (pt) {
                    if (pt->state == THREAD_BLOCKED) {
                        pt->state = THREAD_RUNNING;
                        sched_add(pt);
                    }
                    pt = pt->proc_next;
                }
                spin_unlock_irq(&parent->lock, pflags);
            }
        }
        return 0;
    }

    /* User handler registered — set pending bit.
     * Delivery happens on return to userspace via check_pending_signals. */
    target->sig_pending |= (1ULL << sig);

    /* Wake blocked threads that have this signal unblocked.
     * Lock target to make state=RUNNING + sched_add atomic. */
    {
        extern void sched_add(thread_t *t);
        uint64_t tflags;
        spin_lock_irq(&target->lock, &tflags);
        thread_t *t = target->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED && !((1ULL << sig) & t->sig_blocked)) {
                t->state = THREAD_RUNNING;
                sched_add(t);
            }
            t = t->proc_next;
        }
        spin_unlock_irq(&target->lock, tflags);
    }
    return 0;
}

/* Send signal to all processes in a process group. */
static long kill_pgrp(uint32_t pgid, int sig) {
    int found = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = &proc_pool[i];
        if (p->state == PROC_ALIVE && p->pgid == pgid) {
            kill_one(p, sig);
            found = 1;
        }
    }
    return found ? 0 : -ESRCH;
}

long do_kill(int pid, int sig) {
    if (sig < 0 || sig >= 64) return -EINVAL;
    if (sig == 0) return 0; /* check permission only */

    if (pid > 0) {
        /* Signal to specific process */
        process_t *target = proc_find((uint32_t)pid);
        if (!target) return -ESRCH;
        return kill_one(target, sig);
    } else if (pid == 0) {
        /* Signal to own process group */
        process_t *self = proc_current();
        if (!self) return -ESRCH;
        return kill_pgrp(self->pgid, sig);
    } else if (pid == -1) {
        /* Signal to all processes except init (pid 1) and self */
        extern process_t proc_pool[];
        process_t *self = proc_current();
        int found = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *p = &proc_pool[i];
            if (p->state != PROC_ALIVE) continue;
            if (p->pid <= 1) continue; /* skip init */
            kill_one(p, sig);
            found = 1;
        }
        /* Also signal self */
        if (self) kill_one(self, sig);
        return found || self ? 0 : -ESRCH;
    } else {
        /* pid < -1: signal to process group abs(pid) */
        return kill_pgrp((uint32_t)(-pid), sig);
    }
}

/* ── SYS_TGKILL (234) — thread-directed signal ────── */

long do_tgkill(int tgid, int tid, int sig) {
    if (sig < 0 || sig >= 64) return -EINVAL;
    if (tgid <= 0 || tid <= 0) return -EINVAL;
    if (sig == 0) return 0; /* permission check only */

    /* Find target thread */
    thread_t *target = thread_find_by_tid(tid);
    if (!target || !target->proc) return -ESRCH;
    if ((int)target->proc->pid != tgid) return -EINVAL; /* tid not in tgid */

    process_t *p = target->proc;

    /* Check handler */
    void *handler = p->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    if (handler == SIG_DFL) {
        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, SIGIO */
        if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGIO) return 0;
        /* Stop: SIGTSTP, SIGTTIN, SIGTTOU — ignore for now */
        if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) return 0;
        /* Continue: SIGCONT — ignore for now */
        if (sig == SIGCONT) return 0;

        /* Everything else: terminate */
        p->exit_signal = sig;
        if (p == proc_current()) {
            do_exit_group(128 + sig);
        }
        p->state = PROC_ZOMBIE;
        p->exit_code = 128 + sig;
        {
            thread_t *th = p->threads;
            while (th) {
                if (th->state == THREAD_BLOCKED || th->state == THREAD_RUNNING)
                    th->state = THREAD_DEAD;
                th = th->proc_next;
            }
        }
        /* Wake parent if blocked in wait4 */
        if (p->parent_pid) {
            process_t *parent = proc_find(p->parent_pid);
            if (parent) {
                extern void sched_add(thread_t *t);
                thread_t *pt = parent->threads;
                while (pt) {
                    if (pt->state == THREAD_BLOCKED) {
                        pt->state = THREAD_RUNNING;
                        sched_add(pt);
                    }
                    pt = pt->proc_next;
                }
            }
        }
        return 0;
    }

    /* User handler — set pending and wake target thread */
    p->sig_pending |= (1ULL << sig);
    if (!((1ULL << sig) & target->sig_blocked)) {
        if (target->state == THREAD_BLOCKED) {
            extern void sched_add(thread_t *t);
            target->state = THREAD_RUNNING;
            sched_add(target);
        }
    }
    return 0;
}

/* ── SYS_RT_SIGRETURN (15) ──────────────────────────── */

long do_rt_sigreturn(void) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;
    if (!t || !t->proc) return -EFAULT;

    /* After the handler did `ret`, RSP points past the return address.
     * The restorer then called `syscall` for SYS_RT_SIGRETURN.
     * The user RSP at syscall entry = restorer's RSP.
     * But the restorer is a simple `mov rax,15; syscall` — no stack ops.
     * So user RSP = frame base + 8 (return addr was popped by handler's `ret`).
     * We need to find the signal frame at user_rsp - 8. */
    uint64_t frame_rsp = cpu->user_rsp - 8;

    /* Read ucontext from the signal frame (direct access — no SMAP) */
    uint64_t uc_addr = frame_rsp + SIGFRAME_OFF_UCONTEXT;
    sig_ucontext_t uc;
    { int r = copy_from_user(&uc, (const void *)uc_addr, sizeof(uc)); if (r) return r; }

    /* Restore FPU/SSE state from inline __fpregs_mem via FXRSTOR.
     * FXRSTOR requires 16-byte aligned operand, so use a temp buffer. */
    {
        sig_fpstate_t _fxbuf __attribute__((aligned(16)));
        kmemcpy(&_fxbuf, &uc.__fpregs_mem, sizeof(sig_fpstate_t));
        __asm__ volatile("fxrstor %0" : : "m"(_fxbuf));
    }

    /* Restore registers from ucontext gregset into syscall frame.
     * The SYSRET epilog in syscall_entry.asm will pop these. */
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    frame->r15 = uc.uc_mcontext.gregs.r15; frame->r14 = uc.uc_mcontext.gregs.r14;
    frame->r13 = uc.uc_mcontext.gregs.r13; frame->r12 = uc.uc_mcontext.gregs.r12;
    frame->rbp = uc.uc_mcontext.gregs.rbp; frame->rbx = uc.uc_mcontext.gregs.rbx;
    frame->r9  = uc.uc_mcontext.gregs.r9;  frame->r8  = uc.uc_mcontext.gregs.r8;
    frame->r10 = uc.uc_mcontext.gregs.r10; frame->rdx = uc.uc_mcontext.gregs.rdx;
    frame->rsi = uc.uc_mcontext.gregs.rsi; frame->rdi = uc.uc_mcontext.gregs.rdi;
    frame->rax = uc.uc_mcontext.gregs.rax;
    /* Validate RIP/RSP — must be in user space to prevent kernel exec */
    uint64_t new_rip = uc.uc_mcontext.gregs.rip;
    uint64_t new_rsp = uc.uc_mcontext.gregs.rsp;
    if (new_rip >= 0x800000000000ULL || new_rsp >= 0x800000000000ULL)
        return -EFAULT;
    /* Sanitize RFLAGS — clear IOPL, VM, RF, keep only safe flags */
    uint64_t new_flags = uc.uc_mcontext.gregs.eflags;
    new_flags &= 0x000000000000FFFFULL; /* keep lower 16 bits */
    new_flags &= ~(3ULL << 12);         /* clear IOPL */
    new_flags |= (1ULL << 9);           /* force IF=1 (interrupts enabled) */
    new_flags |= (1ULL << 1);           /* reserved bit 1 always set */
    frame->r11 = new_flags;
    frame->rcx = new_rip;
    cpu->user_rsp = new_rsp;

    /* Restore FS_BASE from _reserved[0] (saved in deliver_signal).
     * Validate: must be user-space address, kernel addresses rejected. */
    {
        uint64_t fs = uc.uc_mcontext._reserved[0];
        if (fs >= 0x800000000000ULL) fs = 0;
        if (fs) {
            t->fs_base = fs;
            __asm__ volatile("wrmsr" :: "c"(0xC0000100),
                             "a"((uint32_t)fs), "d"((uint32_t)(fs >> 32)));
        }
    }

    /* Restore signal mask (first word of 128-byte sigset) */
    t->sig_blocked = uc.uc_sigmask[0];
    t->sig_blocked &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL/SIGSTOP never blocked */

    /* Return value doesn't matter — RAX is restored from ucontext.
     * But the syscall_entry.asm overwrites RAX with our return value AFTER
     * we set frame->rax. We need RAX to be the saved value.
     * Trick: return the saved RAX so it ends up correct. */
    return (long)uc.uc_mcontext.gregs.rax;
}

/* ── SYS_RT_SIGSUSPEND (130) ────────────────────────── */

long do_rt_sigsuspend(const uint64_t *mask, size_t sigsetsize) {
    if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    process_t *p = t->proc;
    if (!p) return -EFAULT;
    if (!mask) return -EFAULT;
    uint64_t new_mask;
    { int r = copy_from_user(&new_mask, mask, 8); if (r) return r; }
    new_mask &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL/SIGSTOP never blocked */

    uint64_t old_blocked = t->sig_blocked;

    /* Install temporary mask */
    t->sig_blocked = new_mask;

    /* If a signal is already deliverable, restore and return */
    if (p->sig_pending & ~t->sig_blocked) {
        t->sig_blocked = old_blocked;
        return -EINTR;
    }

    /* Save old mask on thread so signal delivery path restores it */
    t->sig_saved_mask = old_blocked;
    t->in_sigsuspend = 1;

    /* Block — woken by do_kill when signal becomes deliverable */
    extern uint64_t pml4[];
    save_user_state_for_block(t, -EINTR);
    t->state = THREAD_BLOCKED;
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
    thread_return_to_kernel(t);
    return -EINTR; /* unreachable */
}

/* Check and deliver signals in the SYSCALL return path.
 * Syncs percpu syscall frame <-> thread_t around delivery.
 * Called from syscall_entry.asm between sys_handler return and SYSRET.
 * Actually called from the ASM-adjacent C code. */
/* Is this syscall restartable when interrupted by a signal with SA_RESTART? */
static int is_restartable_syscall(long num) {
    return num == SYS_READ || num == SYS_WRITE || num == SYS_READV ||
           num == SYS_WRITEV || num == SYS_WAIT4 || num == SYS_NANOSLEEP ||
           num == SYS_CLOCK_NANOSLEEP || num == SYS_POLL ||
           num == SYS_RECVFROM || num == SYS_SENDTO ||
           num == SYS_RECVMSG || num == SYS_SENDMSG ||
           num == SYS_ACCEPT || num == SYS_ACCEPT4 ||
           num == SYS_CONNECT;
}

void check_signals_syscall_path(long *result_ptr, long num) {
    if (num == SYS_RT_SIGRETURN) return;
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;
    uint64_t deliverable = p->sig_pending & ~t->sig_blocked;
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

    /* SA_RESTART: if syscall returned -EINTR and the about-to-be-delivered
     * signal has SA_RESTART, set up registers so rt_sigreturn restarts the
     * syscall instead of returning -EINTR. */
    if (*result_ptr == -EINTR && is_restartable_syscall(num)) {
        /* Find which signal will be delivered */
        for (int sig = 1; sig < 64; sig++) {
            if (!(deliverable & (1ULL << sig))) continue;
            struct k_sigaction *sa = &p->sig_actions[sig];
            if ((uint64_t)sa->sa_handler > 1 && (sa->sa_flags & SA_RESTART)) {
                /* Rewind RIP to re-execute syscall instruction.
                 * Set RAX to syscall number so it re-enters correctly. */
                t->rip -= 2;        /* back over `syscall` (0F 05) */
                t->rax = (uint64_t)num;
            }
            break; /* only first deliverable signal matters */
        }
    }

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
