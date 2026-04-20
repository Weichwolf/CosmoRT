/* Kernel exception table — Linux-style user-memory-access fault recovery.
 *
 * Each entry pairs (fault_pc, fixup_pc) as 32-bit offsets relative to the
 * entry's own address. Encoding matches Linux arch/x86 extable layout:
 *
 *     struct ex_entry { s32 insn; s32 fixup; s32 data; };  // 12 bytes
 *
 * Inline-asm user-access sequences annotate each faulting instruction via
 * _ASM_EXTABLE(fault_label, fixup_label). Page-fault handler binary-searches
 * the sorted section; on hit it sets frame->rip = fixup_pc and returns — no
 * registers or FPU state touched. Fixup code sets -EFAULT and jumps to the
 * epilog of the enclosing C function. */
#ifndef KERNEL_EXTABLE_H
#define KERNEL_EXTABLE_H

#include <stdint.h>
#include <stddef.h>

struct ex_entry {
    int32_t insn;
    int32_t fixup;
    int32_t data;
};

_Static_assert(sizeof(struct ex_entry) == 12,
               "ex_entry must be exactly 12 bytes for Linux-compatible layout");

extern struct ex_entry __start_ex_table[];
extern struct ex_entry __stop_ex_table[];

/* Resolve faulting instruction pointer to fixup address.
 * Returns 0 if no fixup registered (caller falls through to signal/panic). */
uintptr_t extable_lookup(uintptr_t fault_pc);

/* One-shot boot-time sort by fault_pc so extable_lookup can binary-search.
 * Must run before the first user-memory access; called from kmain early. */
void extable_sort(void);

/* _ASM_EXTABLE(from, to) — emit a fault/fixup pair into the .ex_table
 * section. Both labels are rel32 offsets relative to their entry slot.
 * Usage in inline asm:
 *     "1: movq (%[src]), %%rax\n"
 *     "2:\n"
 *     _ASM_EXTABLE(1b, 3f)
 *     ".section .text.fixup, \"ax\"\n"
 *     "3: movl $-EFAULT, %[ret]\n"
 *     "   jmp 2b\n"
 *     ".previous\n"
 *
 * .balign 4: 32-bit rel offsets need natural alignment.
 * .previous: restore prior section after emission. */
#define _ASM_EXTABLE(from, to)              \
    ".pushsection .ex_table, \"a\"\n"       \
    ".balign 4\n"                           \
    ".long (" #from ") - .\n"               \
    ".long (" #to ")   - .\n"               \
    ".long 0\n"                             \
    ".popsection\n"

#endif
