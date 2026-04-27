/* CosmoRT Syscall Layer — signal frame setup/teardown, sigreturn */

#include "sys/internal.h"

/* sigaltstack flags */
#define SS_ONSTACK 1
#define SS_DISABLE 2

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

/* Signal frame layout (Linux x86_64 ABI):
 *
 *   Original RSP
 *     [128-byte red zone]
 *     [fpstate: xsave_size bytes, 64-byte aligned]  ← uc_mcontext.fpstate
 *     [trampoline: 9 bytes]  (if no sa_restorer)
 *     [ucontext_t: 936 bytes]
 *     [siginfo_t: 128 bytes]
 *     [return address: 8 bytes]  ← new RSP (16-byte aligned - 8)
 *
 * fpstate is placed above the frame (higher addresses), separately from
 * ucontext_t.__fpregs_mem, and contains the full XSAVE image.
 */

/* Base frame (without fpstate): retaddr + siginfo + ucontext + trampoline */
#define SIG_BASE_FRAME  (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t) + sizeof(sig_trampoline))

#define SIGFRAME_OFF_RETADDR   0
#define SIGFRAME_OFF_SIGINFO   8
#define SIGFRAME_OFF_UCONTEXT  (8 + sizeof(sig_siginfo_t))
#define SIGFRAME_OFF_TRAMPOLINE (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t))

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

/* Deliver signal to user thread by pushing signal frame onto user stack.
 * Modifies thread registers so next return-to-userspace enters the handler.
 * Called from check_pending_signals (SYSCALL return or timer preempt path). */
void deliver_signal(thread_t *t, int signo) {
    process_t *p = t->proc;
    struct k_sigaction *sa = &p->sig_actions[signo];

    if ((uint64_t)sa->sa_handler <= 1) return; /* SIG_DFL or SIG_IGN — shouldn't be here */

    /* Validate handler address: must be in user space */
    uint64_t handler_addr = (uint64_t)sa->sa_handler;
    if (handler_addr >= 0x800000000000ULL) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

    int has_restorer = (sa->sa_flags & SA_RESTORER) && sa->sa_restorer;

    /* Save full FPU/SSE/AVX state to thread (consistent snapshot) */
    hal_cpu_fpu_save(t->xsave_area);

    /* Frame layout: fpstate (64-byte aligned) above base frame.
     * fpstate_size is the actual XSAVE area size, 64-byte aligned. */
    uint64_t fpstate_padded = (xsave_size + 63) & ~63ULL;
    uint64_t base_size = SIG_BASE_FRAME;
    if (has_restorer) base_size -= sizeof(sig_trampoline);

    uint64_t stack_rsp = t->rsp;
    if ((sa->sa_flags & SA_ONSTACK) && t->sigalt_sp &&
        !(t->sigalt_flags & SS_DISABLE)) {
        if (t->rsp < t->sigalt_sp || t->rsp >= t->sigalt_sp + t->sigalt_size)
            stack_rsp = t->sigalt_sp + t->sigalt_size;
    }

    uint64_t total_size = 128 + fpstate_padded + base_size + 64;
    if (stack_rsp < total_size) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

    /* Allocate fpstate at top (just below red zone), 64-byte aligned */
    uint64_t sp = stack_rsp - 128;
    sp = (sp - fpstate_padded) & ~63ULL;
    uint64_t fpstate_user_addr = sp;

    /* Allocate base frame below fpstate */
    uint64_t new_rsp = ((sp - base_size) & ~0xFULL) - 8;

    /* Verify entire range [new_rsp .. fpstate + xsave_size) is writable */
    uint64_t frame_top = fpstate_user_addr + xsave_size;
    vma_t *vma = vma_find(p->vma_root, new_rsp);
    if (!vma || new_rsp < vma->start || frame_top > vma->end
        || !(vma->prot & PROT_WRITE)) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

    for (uint64_t addr = new_rsp & ~0xFFFULL; addr < frame_top; addr += 4096) {
        if (ensure_user_page(p, addr) < 0) {
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

    /* Save FS/GS_BASE in _reserved[0..1] for restoration in rt_sigreturn */
    uc.uc_mcontext._reserved[0] = t->fs_base;
    uc.uc_mcontext._reserved[1] = t->gs_base;

    /* fpstate pointer → separate area on stack (full XSAVE image) */
    uc.uc_mcontext.fpstate = fpstate_user_addr;

    /* Legacy 512 bytes in __fpregs_mem for backward compat (musl getcontext) */
    kmemcpy(&uc.__fpregs_mem, t->xsave_area, sizeof(sig_fpstate_t));

    uc.uc_sigmask[0] = t->sig_blocked;

    /* Build siginfo */
    sig_siginfo_t si;
    kmemset(&si, 0, sizeof(si));
    si.si_signo = (int32_t)signo;
    if (signo == SIGSEGV || signo == SIGBUS) {
        si.si_code = 1; /* SEGV_MAPERR */
        uint64_t fault = t->fault_addr;
        kmemcpy(&si._pad[0], &fault, 8);
    } else {
        /* Linux siginfo_t fuer POLL/dnotify: si_code = SI_POLL (-6),
         * si_band im ersten _pad-Slot, si_fd im zweiten (Layout-Offsets
         * entsprechen siginfo_t._sifields._sigpoll.{si_band=long, si_fd=int}).
         * Unser sig_siginfo_t hat _pad ab Byte 16: bytes 16-23 = band,
         * bytes 24-27 = fd. */
        extern int dnotify_queue_pop_fd(process_t *p, int sig);
        extern int dnotify_queue_peek_fd(process_t *p, int sig);
        int fd = dnotify_queue_pop_fd(p, signo);
        if (fd >= 0) {
            si.si_code = -6; /* SI_POLL (Linux include/uapi/asm-generic/siginfo.h) */
            long band = 0;
            kmemcpy(&si._pad[0], &band, 8);
            kmemcpy(&si._pad[8], &fd, 4);
            /* Linux behandelt dnotify als RT-signal: mehrfach gleichzeitig
             * queueable. Unser sig_pending ist 1-Bit — Re-Pend wenn weitere
             * Eintraege fuer diesen sig vorhanden, damit naechster Iteration
             * von check_pending_signals den naechsten fd ausliefert. */
            if (dnotify_queue_peek_fd(p, signo) >= 0)
                __sync_fetch_and_or(&p->sig_pending, SIG_BIT(signo));
        } else {
            si.si_code = 0; /* SI_USER */
        }
    }

    uint64_t restorer_addr;
    if (has_restorer) {
        restorer_addr = (uint64_t)sa->sa_restorer;
    } else {
        restorer_addr = new_rsp + SIGFRAME_OFF_TRAMPOLINE;
    }

    /* Write signal frame + fpstate to user stack.
     * SMAP bracket: kernel temporarily allowed to touch user pages. */
    hal_cpu_user_access_begin();
    *(uint64_t *)new_rsp = restorer_addr;
    kmemcpy((void *)(new_rsp + SIGFRAME_OFF_SIGINFO), &si, sizeof(si));
    kmemcpy((void *)(new_rsp + SIGFRAME_OFF_UCONTEXT), &uc, sizeof(uc));
    /* Write full XSAVE image to fpstate area */
    kmemcpy((void *)fpstate_user_addr, t->xsave_area, xsave_size);
    if (!has_restorer)
        kmemcpy((void *)(new_rsp + SIGFRAME_OFF_TRAMPOLINE), sig_trampoline, sizeof(sig_trampoline));
    hal_cpu_user_access_end();

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
        t->sig_blocked |= SIG_BIT(signo);
    t->sig_blocked |= sa->sa_mask;
    /* SIGKILL/SIGSTOP never blocked */
    t->sig_blocked &= ~(SIG_BIT(9) | SIG_BIT(19));

    /* SA_RESETHAND: one-shot handler, reset to SIG_DFL after delivery */
    if (sa->sa_flags & SA_RESETHAND)
        sa->sa_handler = SIG_DFL;
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

    /* Restore full FPU/SSE/AVX state from fpstate on user stack.
     * fpstate pointer was set by deliver_signal to the separate XSAVE area. */
    {
        uint64_t fp_addr = uc.uc_mcontext.fpstate;
        if (fp_addr >= 0x800000000000ULL) return -EFAULT;
        int r = copy_from_user(t->xsave_area, (const void *)fp_addr, xsave_size);
        if (r) return r;
        /* Sanitize MXCSR: clear reserved bits, prevent unmasked FP exceptions */
        uint32_t mxcsr = *(uint32_t *)(t->xsave_area + 24);
        uint32_t mxcsr_mask = *(uint32_t *)(t->xsave_area + 28);
        if (mxcsr_mask == 0) mxcsr_mask = 0x0000FFBF;
        *(uint32_t *)(t->xsave_area + 24) = mxcsr & mxcsr_mask;
        hal_cpu_fpu_restore(t->xsave_area);
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

    /* Restore FS/GS_BASE from _reserved[0..1] (saved in deliver_signal).
     * Validate: must be user-space address, kernel addresses rejected. */
    {
        uint64_t fs = uc.uc_mcontext._reserved[0];
        if (fs >= 0x800000000000ULL) fs = 0;
        if (fs) {
            t->fs_base = fs;
            hal_cpu_set_tls(fs);
        }
        uint64_t gs = uc.uc_mcontext._reserved[1];
        if (gs >= 0x800000000000ULL) gs = 0;
        if (gs) {
            t->gs_base = gs;
            hal_cpu_set_user_gs(gs);
        }
    }

    /* Restore signal mask (first word of 128-byte sigset) */
    t->sig_blocked = uc.uc_sigmask[0];
    t->sig_blocked &= ~(SIG_BIT(9) | SIG_BIT(19)); /* SIGKILL/SIGSTOP never blocked */

    /* Return value doesn't matter — RAX is restored from ucontext.
     * But the syscall_entry.asm overwrites RAX with our return value AFTER
     * we set frame->rax. We need RAX to be the saved value.
     * Trick: return the saved RAX so it ends up correct. */
    return (long)uc.uc_mcontext.gregs.rax;
}
