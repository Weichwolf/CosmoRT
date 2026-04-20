/* Kernel exception table — runtime lookup + boot-time sort.
 *
 * Layout: see include/kernel/mm/extable.h. Entries live in .ex_table section
 * bracketed by __start_ex_table / __stop_ex_table. Section is unordered as
 * emitted by the assembler (source-order per translation unit); extable_sort
 * orders it by absolute fault_pc so extable_lookup can binary-search.
 *
 * Offsets are rel32 to the entry's own address — sorting must patch the
 * offsets accordingly because swapping two entries changes the distance
 * between the stored offset and the instruction it refers to. */

#include "mm/extable.h"
#include "linux/abi.h"

static inline uintptr_t ex_insn_addr(const struct ex_entry *e) {
    return (uintptr_t)&e->insn + (intptr_t)e->insn;
}

static inline uintptr_t ex_fixup_addr(const struct ex_entry *e) {
    return (uintptr_t)&e->fixup + (intptr_t)e->fixup;
}

__attribute__((cold))
static void ex_swap(struct ex_entry *a, struct ex_entry *b) {
    uintptr_t a_insn  = ex_insn_addr(a);
    uintptr_t a_fixup = ex_fixup_addr(a);
    uintptr_t b_insn  = ex_insn_addr(b);
    uintptr_t b_fixup = ex_fixup_addr(b);
    int32_t   a_data  = a->data;
    int32_t   b_data  = b->data;

    a->insn  = (int32_t)((intptr_t)b_insn  - (intptr_t)&a->insn);
    a->fixup = (int32_t)((intptr_t)b_fixup - (intptr_t)&a->fixup);
    a->data  = b_data;

    b->insn  = (int32_t)((intptr_t)a_insn  - (intptr_t)&b->insn);
    b->fixup = (int32_t)((intptr_t)a_fixup - (intptr_t)&b->fixup);
    b->data  = a_data;
}

__attribute__((cold))
void extable_sort(void) {
    struct ex_entry *start = __start_ex_table;
    struct ex_entry *stop  = __stop_ex_table;
    size_t n = (size_t)(stop - start);
    if (n < 2) return;

    /* Insertion sort: extable size is O(100) entries, runs once at boot,
     * and stability / absence of recursion matter more than complexity. */
    for (size_t i = 1; i < n; i++) {
        uintptr_t key = ex_insn_addr(&start[i]);
        size_t j = i;
        while (j > 0 && ex_insn_addr(&start[j - 1]) > key) {
            ex_swap(&start[j], &start[j - 1]);
            j--;
            key = ex_insn_addr(&start[j]);
        }
    }
}

__attribute__((cold))
uintptr_t extable_lookup(uintptr_t fault_pc) {
    const struct ex_entry *start = __start_ex_table;
    const struct ex_entry *stop  = __stop_ex_table;
    size_t lo = 0;
    size_t hi = (size_t)(stop - start);

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uintptr_t insn = ex_insn_addr(&start[mid]);
        if (insn == fault_pc) return ex_fixup_addr(&start[mid]);
        if (insn <  fault_pc) lo = mid + 1;
        else                  hi = mid;
    }
    return 0;
}
