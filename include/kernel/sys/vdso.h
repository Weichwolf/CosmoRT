/* CosmoRT vDSO — kernel-shared timing data + user-space code page.
 *
 * Userspace reads vdso_data via seqlock: retry while seq is odd or
 * changed during the read. Kernel writes under vdso_data_update().
 *
 * Layout in user-mm (fixed VAs, applied per process at exec):
 *   VDSO_DATA_VA  : 1 page, PROT_READ          — kernel-updated timing scale
 *   VDSO_CODE_VA  : 1 page, PROT_READ|PROT_EXEC — ELF DSO mit __vdso_clock_gettime
 *
 * AT_SYSINFO_EHDR auxv entry points at VDSO_CODE_VA, so musl's dlsym-style
 * lookup parses the embedded ELF and resolves __vdso_clock_gettime.
 */
#ifndef KERNEL_SYS_VDSO_H
#define KERNEL_SYS_VDSO_H

#include <stdint.h>

/* Userspace VAs for both vDSO pages. Placed between USER_MMAP_BASE
 * (0x7F0000000000) and the maximum user-stack range (USER_STACK_TOP -
 * RLIM_STACK_MAX = 0x7FFFFC000000). 0x7FFFEFFFE000 + 8KB sits comfortably
 * in that gap and is reserved by VMAs so mm_mmap won't allocate there.
 * Both pages are 4KB-aligned and fixed system-wide. */
#define VDSO_DATA_VA   0x7FFFEFFFE000ULL
#define VDSO_CODE_VA   0x7FFFEFFFF000ULL

/* WARNING: keep in sync with vdso.c userspace assembly. Any change is
 * an ABI break — every running process has the old offsets baked in. */
struct vdso_data {
    uint32_t seq;             /* seqlock: even = stable, odd = writer mid-update */
    uint32_t _pad0;
    uint32_t mult;            /* TSC -> ns: ns = ((tsc - boot) * mult) >> shift */
    uint32_t shift;
    uint64_t boot_tsc;        /* TSC value at kernel boot (subtract before scale) */
    int64_t  wall_time_offset_ns; /* CLOCK_REALTIME = monotonic + this */
    uint64_t version;         /* incremented every kernel-side update */
    uint64_t _pad1[3];        /* pad to 64 bytes for cacheline isolation */
};

_Static_assert(sizeof(struct vdso_data) == 64,
               "vdso_data muss exakt eine Cache-Line sein");

/* Initialize vDSO at boot: allocates code+data pages, copies ELF, writes data.
 * Must run after timer_init() (TSC mult/shift calibrated) and pmm/page_alloc
 * are usable. */
void vdso_init(void);

/* Refresh data page after TSC re-calibration or wall-clock adjustment.
 * Bumps seq odd, writes fields, bumps seq even. Safe vs concurrent readers. */
void vdso_data_update(void);

/* Map vDSO pages into a user PML4 at the canonical VAs and register
 * matching VMAs so mm_mmap won't allocate over them. Called from exec()
 * and fork(). Returns 0 on success.
 *
 * `target_ns` is the time_namespace of the *target* process (the one whose
 * pml4 receives the mapping). Non-init namespaces are refused so musl falls
 * back to the syscall path which honours per-NS offsets. fork-time callers
 * MUST pass the child's namespace, not proc_current()'s — they differ when
 * the parent ran unshare(CLONE_NEWTIME) before fork. */
/* vma_t fully declared in mm/vma.h; forward-declare to avoid pulling kernel
 * mm headers into userspace-shared paths. */
struct vma;
struct time_namespace;
typedef struct vma vma_t_fwd;
int  vdso_map(uint64_t *user_pml4, struct vma **vma_root,
              struct time_namespace *target_ns);

/* Clear vDSO PTEs from a user PML4 without freeing the underlying kernel-
 * owned pages. MUST be called before free_address_space() — otherwise the
 * page_alloc refcount of g_vdso_data/code drops past zero and the next
 * process to use the vDSO sees garbage. */
void vdso_unmap(uint64_t *user_pml4);

/* User-space ELF base of the vDSO code page (for AT_SYSINFO_EHDR auxv).
 * Returns the canonical address only if the vDSO is currently mapped in the
 * supplied PML4 (vdso_map can refuse for non-init time namespaces). The
 * `void` variant queries proc_current()->pml4 — null during early boot. */
uint64_t vdso_user_base(void);
uint64_t vdso_user_base_for(uint64_t *user_pml4);

/* Kernel-side direct-map pointer to the live data page (for ktest verification). */
const struct vdso_data *vdso_data_ptr(void);

#endif
