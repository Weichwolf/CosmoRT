/* CosmoRT vDSO Subsystem
 *
 * Allocates the two physical pages that back the user-visible vDSO at boot,
 * fills the data page with the calibrated TSC scaling parameters, copies
 * the embedded ELF DSO into the code page, and wires both pages into every
 * process address space at exec time.
 *
 * Userspace (musl) finds the DSO via the AT_SYSINFO_EHDR auxv entry that
 * process_exec.c writes at stack-build time. The DSO's __vdso_clock_gettime
 * reads the data page directly — no syscall, no TLB flush. */

#include "internal.h"
#include "sys/vdso.h"
#include "core/time_ns.h"
#include "gen/vdso_bin.h"

/* Physical-page direct-map pointers. Allocated once at boot, never freed.
 *
 * g_vdso_data: live calibration page used by processes in init_time_ns.
 * g_vdso_data_timens: permanently-zeroed sentinel page used by processes in
 *   a non-init time_namespace. The vDSO code returns -ENOSYS when mult==0,
 *   which forces musl onto the syscall path that honours per-NS offsets.
 *   Linux uses a separate VVAR_TIMENS clock_mode for the same purpose. */
static struct vdso_data *g_vdso_data;
static struct vdso_data *g_vdso_data_timens;
static uint8_t           *g_vdso_code;

/* Atomic-store helpers (avoid pulling in core/atomic.h dependency chain) */
static inline void store_u32(volatile uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

const struct vdso_data *vdso_data_ptr(void) { return g_vdso_data; }

/* Return user-space ELF base if `user_pml4` actually has the vDSO mapped.
 * vdso_map() may have refused (non-init time_namespace) — in that case the
 * caller (build_user_stack) skips AT_SYSINFO_EHDR so musl falls back to
 * syscalls. */
uint64_t vdso_user_base_for(uint64_t *user_pml4) {
    if (!user_pml4) return 0;
    uint64_t va = VDSO_CODE_VA;
    int p4i = (va >> 39) & 0x1FF;
    if (!(user_pml4[p4i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[p4i] & PTE_ADDR_MASK);
    int pdpti = (va >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (va >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT) || (pd[pdi] & PTE_PS)) return 0;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    if (!(pt[(va >> 12) & 0x1FF] & PTE_PRESENT)) return 0;
    return VDSO_CODE_VA;
}

uint64_t vdso_user_base(void) {
    process_t *p = proc_current();
    return p ? vdso_user_base_for(p->pml4) : 0;
}

/* ── Data-page update (seqlock writer) ────────────────
 *
 * Bump seq odd → write fields → bump seq even. Readers retry while seq is
 * odd or seq differs across the read. Single writer is the kernel; readers
 * are arbitrary user threads. No lock needed kernel-side since
 * vdso_data_update() runs only from cold paths (boot, recalibration). */
void vdso_data_update(void) {
    if (!g_vdso_data) return;

    uint32_t s = g_vdso_data->seq;
    store_u32(&g_vdso_data->seq, s + 1);          /* odd: writer in progress */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    g_vdso_data->mult     = timer_tsc_to_ns_mult;
    g_vdso_data->shift    = timer_tsc_to_ns_shift;
    g_vdso_data->boot_tsc = timer_boot_tsc;
    g_vdso_data->wall_time_offset_ns =
        (int64_t)rtc_epoch_sec * 1000000000LL;
    g_vdso_data->version++;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    store_u32(&g_vdso_data->seq, s + 2);          /* even: stable again */
}

/* ── Boot init ────────────────────────────────────── */

__attribute__((cold))
void vdso_init(void) {
    g_vdso_data        = (struct vdso_data *)page_alloc();
    g_vdso_data_timens = (struct vdso_data *)page_alloc();
    g_vdso_code        = (uint8_t *)page_alloc();
    if (!g_vdso_data || !g_vdso_data_timens || !g_vdso_code) {
        serial_puts("vdso: page_alloc failed\n");
        g_vdso_data = 0; g_vdso_data_timens = 0; g_vdso_code = 0;
        return;
    }

    /* Zero data page so initial seq=0 (even, stable). The timens sentinel
     * is permanently zero (mult==0) so the vDSO returns -ENOSYS to musl. */
    kmemset(g_vdso_data, 0, 4096);
    kmemset(g_vdso_data_timens, 0, 4096);

    /* Copy the embedded ELF DSO into the code page. vdso_bin_size is the
     * full file length; we only have one page. Build-time check guards the
     * upper bound — if the DSO ever outgrows a page we want a hard fail. */
    _Static_assert(sizeof(vdso_bin) <= 4096,
                   "vDSO image must fit in a single 4KB page");
    kmemset(g_vdso_code, 0, 4096);
    kmemcpy(g_vdso_code, vdso_bin, vdso_bin_size);

    /* Populate data page with current TSC calibration. timer_init() must
     * have run before us. */
    vdso_data_update();

    serial_puts("vdso: ");
    char tmp[16]; int ti = 0;
    unsigned long sz = vdso_bin_size;
    do { tmp[ti++] = (char)('0' + sz % 10); sz /= 10; } while (sz);
    while (ti--) serial_putchar(tmp[ti]);
    serial_puts(" bytes, mapped at code=0x7FFFEFFFF000 data=0x7FFFEFFFE000\n");
}

/* ── Per-process mapping ──────────────────────────── */

int vdso_map(uint64_t *user_pml4, struct vma **vma_root,
             struct time_namespace *target_ns) {
    if (!g_vdso_data || !g_vdso_data_timens || !g_vdso_code || !user_pml4)
        return -1;

    /* Per-process time_namespace offsets (CLONE_NEWTIME) shift CLOCK_MONOTONIC
     * etc. The vDSO data page is global — it can't honour per-process offsets.
     *
     * Strategy: keep the code page mapped unconditionally (musl's __vdso_*
     * pointer cache survives fork, and unmapping causes SIGSEGV the next
     * time it dispatches). Swap the data page to the timens-sentinel
     * (permanently mult=0) so the vDSO returns -ENOSYS to musl, which
     * falls back to the syscall path that honours per-NS offsets. Linux
     * does the same with VVAR_TIMENS clock_mode.
     *
     * NB: target_ns is the child's namespace at fork-time (passed in) —
     * proc_current() points at the parent and would map the wrong page. */
    int use_timens = (target_ns && target_ns != &init_time_ns);
    uint64_t data_pa = virt_to_phys(use_timens ? g_vdso_data_timens : g_vdso_data);
    uint64_t code_pa = virt_to_phys(g_vdso_code);

    /* Data: read-only, NX. Code: read+execute, NX off. Both shared across
     * all processes — they map the same physical page. */
    if (map_user_page(user_pml4, VDSO_DATA_VA, data_pa, PROT_READ) < 0)
        return -1;
    if (map_user_page(user_pml4, VDSO_CODE_VA, code_pa,
                      PROT_READ | PROT_EXEC) < 0)
        return -1;

    /* Reserve VA range so mm_mmap won't allocate over the vDSO and
     * /proc/self/maps reports it. fork copy_one_vma SKIPs MAP_VDSO so the
     * canonical mapping is the only source. */
    if (vma_root) {
        vma_insert(vma_root, VDSO_DATA_VA, VDSO_DATA_VA + 4096,
                   PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_VDSO);
        vma_insert(vma_root, VDSO_CODE_VA, VDSO_CODE_VA + 4096,
                   PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED | MAP_VDSO);
    }
    return 0;
}

/* Walk to the leaf PTE for `va` in `user_pml4` and clear it (no free). */
static void clear_leaf_pte(uint64_t *user_pml4, uint64_t va) {
    int p4i = (va >> 39) & 0x1FF;
    if (!(user_pml4[p4i] & PTE_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[p4i] & PTE_ADDR_MASK);
    int pdpti = (va >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (va >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT) || (pd[pdi] & PTE_PS)) return;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    pt[(va >> 12) & 0x1FF] = 0;
}

void vdso_unmap(uint64_t *user_pml4) {
    if (!user_pml4) return;
    clear_leaf_pte(user_pml4, VDSO_DATA_VA);
    clear_leaf_pte(user_pml4, VDSO_CODE_VA);
}
