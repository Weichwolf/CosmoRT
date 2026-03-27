/* CosmoRT Syscall Layer — signal frame setup/teardown, sigreturn */

#include "internal.h"
#include "core/event_queue.h"

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

    /* Validate handler address: must be in user address space.
     * Garbage handlers (from fuzzing rt_sigaction) would cause the CPU to
     * jump to unmapped/kernel memory on return to userspace. */
    uint64_t handler_addr = (uint64_t)sa->sa_handler;
    if (handler_addr >= 0x800000000000ULL) {
        /* Invalid handler — kill process instead of jumping to garbage */
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

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
    /* Guard against RSP underflow (stack_rsp too small for red zone + frame) */
    if (stack_rsp < 128 + frame_size + 8) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
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
        arch_fxsave(&_fxbuf);
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
     * FXRSTOR requires 16-byte aligned operand, so use a temp buffer.
     * Validate MXCSR first: reserved bits (31:16) must be zero, and
     * each mask bit must cover the corresponding exception-enable bit
     * (otherwise FXRSTOR raises #GP in kernel mode — fatal). */
    {
        sig_fpstate_t _fxbuf __attribute__((aligned(16)));
        kmemcpy(&_fxbuf, &uc.__fpregs_mem, sizeof(sig_fpstate_t));
        /* Sanitize MXCSR: clear reserved bits, force mask bits on to
         * prevent unmasked FP exceptions in kernel context */
        uint32_t mxcsr = _fxbuf.mxcsr;
        uint32_t mxcsr_mask = _fxbuf.mxcsr_mask;
        if (mxcsr_mask == 0) mxcsr_mask = 0x0000FFBF; /* default if unsupported */
        _fxbuf.mxcsr = mxcsr & mxcsr_mask;
        arch_fxrstor(&_fxbuf);
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
            arch_set_fs_base(fs);
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
