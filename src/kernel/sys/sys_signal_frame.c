/* CosmoRT Syscall Layer — signal frame setup/teardown, sigreturn */

#include "internal.h"
#include "core/event_queue.h"

#define SS_ONSTACK 1
#define SS_DISABLE 2

typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint64_t csgsfs;
    uint64_t err, trapno, oldmask, cr2;
} sig_gregset_t;

typedef struct {
    uint16_t cwd, swd, twd, fop;
    uint64_t rip, rdp;
    uint32_t mxcsr, mxcsr_mask;
    uint8_t  st_space[128];
    uint8_t  xmm_space[256];
    uint8_t  _reserved[96];
} sig_fpstate_t;

typedef struct {
    sig_gregset_t gregs;
    uint64_t      fpstate;
    uint64_t      _reserved[8];
} sig_mcontext_t;

typedef struct {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
} sig_stack_t;

typedef struct {
    uint64_t       uc_flags;
    uint64_t       uc_link;
    sig_stack_t    uc_stack;
    sig_mcontext_t uc_mcontext;
    uint64_t       uc_sigmask[16];
    sig_fpstate_t  __fpregs_mem;
} sig_ucontext_t;

typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
    uint8_t _pad[128 - 16];
} sig_siginfo_t;

static const uint8_t sig_trampoline[] = {
    0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00,
    0x0f, 0x05
};

#define SIG_FRAME_SIZE  (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t) + sizeof(sig_trampoline))

#define SIGFRAME_OFF_RETADDR   0
#define SIGFRAME_OFF_SIGINFO   8
#define SIGFRAME_OFF_UCONTEXT  (8 + sizeof(sig_siginfo_t))
#define SIGFRAME_OFF_TRAMPOLINE (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t))

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

static int ensure_user_page(process_t *p, uint64_t uaddr) {
    if (resolve_user_addr(p->pml4, uaddr)) return 0;
    uint64_t page_addr = uaddr & ~0xFFFULL;
    uint64_t *page = alloc_page();
    if (!page) return -ENOMEM;
    kmemset(page, 0, 4096);
    return map_user_page(p->pml4, page_addr, virt_to_phys(page), 0x3);
}

void deliver_signal(thread_t *t, int signo) {
    process_t *p = t->proc;
    struct k_sigaction *sa = &p->sig_actions[signo];

    if ((uint64_t)sa->sa_handler <= 1) return;

    uint64_t handler_addr = (uint64_t)sa->sa_handler;
    if (handler_addr >= 0x800000000000ULL) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

    uint64_t frame_size = SIG_FRAME_SIZE;
    int has_restorer = (sa->sa_flags & SA_RESTORER) && sa->sa_restorer;
    if (has_restorer)
        frame_size -= sizeof(sig_trampoline);

    uint64_t stack_rsp = t->rsp;
    if ((sa->sa_flags & SA_ONSTACK) && t->sigalt_sp &&
        !(t->sigalt_flags & SS_DISABLE)) {
        if (t->rsp < t->sigalt_sp || t->rsp >= t->sigalt_sp + t->sigalt_size)
            stack_rsp = t->sigalt_sp + t->sigalt_size;
    }
    if (stack_rsp < 128 + frame_size + 8) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }
    stack_rsp -= 128;
    uint64_t new_rsp = ((stack_rsp - frame_size) & ~0xFULL) - 8;

    vma_t *vma = vma_find(p->vma_root, new_rsp);
    if (!vma || new_rsp < vma->start || (new_rsp + frame_size) > vma->end
        || !(vma->prot & PROT_WRITE)) {
        p->exit_signal = signo;
        do_exit(128 + signo);
        return;
    }

    for (uint64_t addr = new_rsp & ~0xFFFULL; addr < new_rsp + frame_size; addr += 4096) {
        if (ensure_user_page(p, addr) < 0) {
            p->exit_signal = signo;
            do_exit(128 + signo);
            return;
        }
    }

    sig_ucontext_t uc;
    kmemset(&uc, 0, sizeof(uc));

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

    uc.uc_mcontext._reserved[0] = t->fs_base;

    uc.uc_sigmask[0] = t->sig_blocked;

    {
        sig_fpstate_t _fxbuf __attribute__((aligned(16)));
        arch_fxsave(&_fxbuf);
        kmemcpy(&uc.__fpregs_mem, &_fxbuf, sizeof(sig_fpstate_t));
    }

    sig_siginfo_t si;
    kmemset(&si, 0, sizeof(si));
    si.si_signo = (int32_t)signo;
    if (signo == SIGSEGV || signo == SIGBUS) {
        si.si_code = 1;
        uint64_t fault = t->fault_addr;
        kmemcpy(&si._pad[0], &fault, 8);
    } else {
        si.si_code = 0;
    }

    uint64_t restorer_addr;
    if (has_restorer) {
        restorer_addr = (uint64_t)sa->sa_restorer;
    } else {
        restorer_addr = new_rsp + SIGFRAME_OFF_TRAMPOLINE;
    }

    uint64_t uc_user_addr = new_rsp + SIGFRAME_OFF_UCONTEXT;
    uc.uc_mcontext.fpstate = uc_user_addr + __builtin_offsetof(sig_ucontext_t, __fpregs_mem);

    arch_stac();
    *(uint64_t *)new_rsp = restorer_addr;
    kmemcpy((void *)(new_rsp + SIGFRAME_OFF_SIGINFO), &si, sizeof(si));
    kmemcpy((void *)(new_rsp + SIGFRAME_OFF_UCONTEXT), &uc, sizeof(uc));

    if (!has_restorer)
        kmemcpy((void *)(new_rsp + SIGFRAME_OFF_TRAMPOLINE), sig_trampoline, sizeof(sig_trampoline));
    arch_clac();

    t->rip = (uint64_t)sa->sa_handler;
    t->rsp = new_rsp;
    t->rdi = (uint64_t)signo;
    if (sa->sa_flags & SA_SIGINFO) {
        t->rsi = new_rsp + SIGFRAME_OFF_SIGINFO;
        t->rdx = new_rsp + SIGFRAME_OFF_UCONTEXT;
    } else {
        t->rsi = 0;
        t->rdx = 0;
    }
    t->rcx = 0;
    t->r11 = 0;
    t->rflags &= ~(1ULL << 10);
    t->rflags |= (1ULL << 9);

    if (!(sa->sa_flags & SA_NODEFER))
        t->sig_blocked |= SIG_BIT(signo);
    t->sig_blocked |= sa->sa_mask;
    t->sig_blocked &= ~(SIG_BIT(9) | SIG_BIT(19));

    if (sa->sa_flags & SA_RESETHAND)
        sa->sa_handler = SIG_DFL;
}

long do_rt_sigreturn(void) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;
    if (!t || !t->proc) return -EFAULT;

    uint64_t frame_rsp = cpu->user_rsp - 8;

    uint64_t uc_addr = frame_rsp + SIGFRAME_OFF_UCONTEXT;
    sig_ucontext_t uc;
    { int r = copy_from_user(&uc, (const void *)uc_addr, sizeof(uc)); if (r) return r; }

    {
        sig_fpstate_t _fxbuf __attribute__((aligned(16)));
        kmemcpy(&_fxbuf, &uc.__fpregs_mem, sizeof(sig_fpstate_t));
        uint32_t mxcsr = _fxbuf.mxcsr;
        uint32_t mxcsr_mask = _fxbuf.mxcsr_mask;
        if (mxcsr_mask == 0) mxcsr_mask = 0x0000FFBF;
        _fxbuf.mxcsr = mxcsr & mxcsr_mask;
        arch_fxrstor(&_fxbuf);
    }

    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    frame->r15 = uc.uc_mcontext.gregs.r15; frame->r14 = uc.uc_mcontext.gregs.r14;
    frame->r13 = uc.uc_mcontext.gregs.r13; frame->r12 = uc.uc_mcontext.gregs.r12;
    frame->rbp = uc.uc_mcontext.gregs.rbp; frame->rbx = uc.uc_mcontext.gregs.rbx;
    frame->r9  = uc.uc_mcontext.gregs.r9;  frame->r8  = uc.uc_mcontext.gregs.r8;
    frame->r10 = uc.uc_mcontext.gregs.r10; frame->rdx = uc.uc_mcontext.gregs.rdx;
    frame->rsi = uc.uc_mcontext.gregs.rsi; frame->rdi = uc.uc_mcontext.gregs.rdi;
    frame->rax = uc.uc_mcontext.gregs.rax;
    uint64_t new_rip = uc.uc_mcontext.gregs.rip;
    uint64_t new_rsp = uc.uc_mcontext.gregs.rsp;
    if (new_rip >= 0x800000000000ULL || new_rsp >= 0x800000000000ULL)
        return -EFAULT;
    uint64_t new_flags = uc.uc_mcontext.gregs.eflags;
    new_flags &= 0x000000000000FFFFULL;
    new_flags &= ~(3ULL << 12);
    new_flags |= (1ULL << 9);
    new_flags |= (1ULL << 1);
    frame->r11 = new_flags;
    frame->rcx = new_rip;
    cpu->user_rsp = new_rsp;

    {
        uint64_t fs = uc.uc_mcontext._reserved[0];
        if (fs >= 0x800000000000ULL) fs = 0;
        if (fs) {
            t->fs_base = fs;
            arch_set_fs_base(fs);
        }
    }

    t->sig_blocked = uc.uc_sigmask[0];
    t->sig_blocked &= ~(SIG_BIT(9) | SIG_BIT(19));

    return (long)uc.uc_mcontext.gregs.rax;
}
