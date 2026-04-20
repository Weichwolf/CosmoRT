/* IRQ frame ↔ thread_t register sync.
 *
 * Invariants:
 *   - GPRs + RIP/RFLAGS/RSP are copied bidirectionally.
 *   - CS/SS/vector/error are IRQ-stack-frame-only (no thread mirror).
 *   - FS_BASE is NOT copied: it's a CPU register managed by schedule()
 *     (save before block/switch, restore on resume). Signal delivery
 *     round-trips FS_BASE through sigframe._reserved[0], not thread_t.
 *   - FPU state is NOT copied: kernel is compiled -mgeneral-regs-only;
 *     user XSAVE round-trips via thread->xsave_area in schedule().
 */
#ifndef CORE_FRAME_H
#define CORE_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "proc/thread.h"

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} irq_frame_t;

_Static_assert(sizeof(irq_frame_t) == 22 * 8,
               "irq_frame_t layout must match irq_asm.S push order");
_Static_assert(offsetof(irq_frame_t, r15) == 0,  "r15 at base of frame");
_Static_assert(offsetof(irq_frame_t, rax) == 14 * 8, "rax after GPR block");
_Static_assert(offsetof(irq_frame_t, vector) == 15 * 8, "vector slot");
_Static_assert(offsetof(irq_frame_t, error)  == 16 * 8, "error slot");
_Static_assert(offsetof(irq_frame_t, rip)    == 17 * 8, "rip slot");
_Static_assert(offsetof(irq_frame_t, cs)     == 18 * 8, "cs slot");
_Static_assert(offsetof(irq_frame_t, rflags) == 19 * 8, "rflags slot");
_Static_assert(offsetof(irq_frame_t, rsp)    == 20 * 8, "rsp slot");
_Static_assert(offsetof(irq_frame_t, ss)     == 21 * 8, "ss slot");

/* Ring-3 (user-mode) detection: CPL bits in CS. */
static inline int frame_is_user(const irq_frame_t *f) {
    return (f->cs & 3) == 3;
}

static inline void irq_frame_to_thread(const irq_frame_t *f, thread_t *t) {
    t->r15 = f->r15; t->r14 = f->r14; t->r13 = f->r13; t->r12 = f->r12;
    t->r11 = f->r11; t->r10 = f->r10; t->r9  = f->r9;  t->r8  = f->r8;
    t->rbp = f->rbp; t->rdi = f->rdi; t->rsi = f->rsi; t->rdx = f->rdx;
    t->rcx = f->rcx; t->rbx = f->rbx; t->rax = f->rax;
    t->rip = f->rip; t->rflags = f->rflags; t->rsp = f->rsp;
}

static inline void thread_to_irq_frame(const thread_t *t, irq_frame_t *f) {
    f->r15 = t->r15; f->r14 = t->r14; f->r13 = t->r13; f->r12 = t->r12;
    f->r11 = t->r11; f->r10 = t->r10; f->r9  = t->r9;  f->r8  = t->r8;
    f->rbp = t->rbp; f->rdi = t->rdi; f->rsi = t->rsi; f->rdx = t->rdx;
    f->rcx = t->rcx; f->rbx = t->rbx; f->rax = t->rax;
    f->rip = t->rip; f->rflags = t->rflags; f->rsp = t->rsp;
}

#endif
