/* CosmoRT Process — core init, alloc, page tables, context switching
 *
 * Slab-allocated. Each process starts with one main thread.
 * Scheduler operates on threads, not processes.
 */

#include "proc/proc_internal.h"
#include "linux/capability.h"
#include "core/time_ns.h"
#include "net/net_ns.h"

/* Net-NS shim: net layer pulls task->net_ns through this typed accessor
 * so it stays free of proc internals. Strong override of the weak
 * placeholder declared in src/kernel/net/net_ns.c. */
struct net_ns *proc_net_ns(struct process *p) {
    return p ? p->net_ns : 0;
}

/* ── Slab pools ─────────────────────────────────── */

slab_t proc_slab;
slab_t thread_slab;
static int next_pid = 1;
static int next_tid = 1;
spinlock_t pid_lock = SPINLOCK_INIT;

/* O(1) lookup tables — indexed by PID/TID, grow on demand (power-of-two).
 * Start at PID_TABLE_INIT slots (1 page at 8B pointers), double until
 * PID_MAX_CEILING. Growth happens under pid_lock in proc_alloc/thread_alloc. */
#define PID_TABLE_INIT 256
process_t **pid_table;
thread_t  **tid_table;
static int pid_capacity;
static int tid_capacity;

int pid_table_capacity(void) { return pid_capacity; }
int tid_table_capacity(void) { return tid_capacity; }

/* Allocate a pointer-table of `slots` entries via pages_alloc (zeroed).
 * slots must be power-of-two; returns NULL on OOM. */
static void **alloc_ptr_table(int slots) {
    size_t bytes = (size_t)slots * sizeof(void *);
    int npages = (int)((bytes + 4095) / 4096);
    return (void **)pages_alloc(npages);
}

static void free_ptr_table(void **tbl, int slots) {
    size_t bytes = (size_t)slots * sizeof(void *);
    int npages = (int)((bytes + 4095) / 4096);
    pages_free(tbl, npages);
}

/* Grow `*tbl_ptr` from `*cap_ptr` to `*cap_ptr * 2`. Copies existing entries.
 * Caller must hold pid_lock. Returns 0 on success, -1 on OOM or ceiling reached. */
static int grow_ptr_table(void ***tbl_ptr, int *cap_ptr) {
    int old_cap = *cap_ptr;
    if (old_cap >= PID_MAX_CEILING) return -1;
    int new_cap = old_cap * 2;
    if (new_cap > PID_MAX_CEILING) new_cap = PID_MAX_CEILING;
    void **new_tbl = alloc_ptr_table(new_cap);
    if (!new_tbl) return -1;
    for (int i = 0; i < old_cap; i++) new_tbl[i] = (*tbl_ptr)[i];
    void **old_tbl = *tbl_ptr;
    *tbl_ptr = new_tbl;
    *cap_ptr = new_cap;
    free_ptr_table(old_tbl, old_cap);
    return 0;
}

void proc_init(void) {
    slab_init_dynamic(&proc_slab, (int)sizeof(process_t), 32);
    slab_init_dynamic(&thread_slab, (int)sizeof(thread_t), 64);
    pid_table = (process_t **)alloc_ptr_table(PID_TABLE_INIT);
    tid_table = (thread_t  **)alloc_ptr_table(PID_TABLE_INIT);
    if (!pid_table || !tid_table) {
        serial_puts("proc: init OOM on pid/tid table\n");
        for (;;) hal_cpu_halt();
    }
    pid_capacity = PID_TABLE_INIT;
    tid_capacity = PID_TABLE_INIT;
    serial_puts("proc: init (dynamic slab)\n");
}

/* ── Process iteration via pid_table ─────────────── */

void proc_for_each(proc_iter_fn fn, void *ctx) {
    int cap = pid_capacity;
    for (int i = 1; i < cap; i++) {
        process_t *p = pid_table[i];
        if (p && p->state != PROC_FREE && p->pid == (uint32_t)i) {
            if (fn(p, ctx)) return;
        }
    }
}

int proc_count_alive(void) {
    int count = 0;
    int cap = pid_capacity;
    for (int i = 1; i < cap; i++) {
        process_t *p = pid_table[i];
        if (p && p->state == PROC_ALIVE && p->pid == (uint32_t)i)
            count++;
    }
    return count;
}

/* Allocate the next free slot in a dynamic pointer-table.
 * Linux-like monotonic allocator: advance *next_id, wrap at capacity,
 * grow the table (power-of-two) when the wrap finds no free slot.
 * Slot 0 is reserved (PID 0 = kernel/idle); first user slot is 1.
 * Caller must hold pid_lock. Returns slot id (>=1), or -1 on OOM/ceiling. */
static int alloc_next_id(void ***tbl_ptr, int *cap_ptr, int *next_id) {
    for (;;) {
        int cap = *cap_ptr;
        for (int scanned = 0; scanned < cap; scanned++) {
            int candidate = *next_id;
            int n = candidate + 1;
            if (n >= cap) n = (cap >= 2 ? 2 : 1);
            *next_id = n;
            if (candidate >= 1 && candidate < cap && !(*tbl_ptr)[candidate])
                return candidate;
        }
        /* Table fully populated — grow or give up */
        if (grow_ptr_table(tbl_ptr, cap_ptr) < 0) return -1;
        *next_id = cap; /* fresh half starts at old capacity */
    }
}

thread_t *thread_alloc(void) {
    thread_t *t = (thread_t *)slab_alloc(&thread_slab);
    if (t) {
        /* Zero entire struct: slab may reuse slots with stale data
         * (e.g. sig_thread_pending from a dead thread → spurious signals). */
        kmemset(t, 0, sizeof(thread_t));

        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        int tid = alloc_next_id((void ***)&tid_table, &tid_capacity, &next_tid);
        if (tid < 0) {
            spin_unlock_irq(&pid_lock, flags);
            slab_free(&thread_slab, t);
            return 0;
        }
        t->tid = tid;
        tid_table[tid] = t;
        spin_unlock_irq(&pid_lock, flags);
    }
    return t;
}

void thread_free(thread_t *t) {
    if (!t) return;
    /* Drop t out of any run queue first so other threads' rq_next pointers
     * stop dangling into the freed slab slot. Without this the singly-linked
     * predecessor keeps a pointer past the (about to be reused) slot, and
     * sched_add's idempotency check on that predecessor returns spuriously
     * true after the slot reuses. */
    extern void sched_dequeue(thread_t *t);
    sched_dequeue(t);
    /* Defence in depth: futex_thread_exit is also called from do_exit, but
     * threads killed via signal/group-exit may reach thread_free without
     * having executed do_exit. Unlinking a stale stack-allocated waiter
     * before the kstack is freed is mandatory to prevent UAF in the bucket
     * doubly-linked list. Idempotent (NULL t->futex_waiter is the no-op). */
    extern void futex_thread_exit(thread_t *t);
    futex_thread_exit(t);
    extern int hrtimer_cancel_by_data(void *);
    hrtimer_cancel_by_data(t);
    if (t->tid > 0 && t->tid < tid_capacity)
        tid_table[t->tid] = 0;
    if (t->kstack)
        pages_free(t->kstack, KSTACK_SIZE / 4096);
    slab_free(&thread_slab, t);
}

process_t *proc_alloc(void) {
    process_t *p = (process_t *)slab_alloc(&proc_slab);
    if (p) {
        kmemset(p, 0, sizeof(process_t));

        uint64_t flags;
        spin_lock_irq(&pid_lock, &flags);
        int pid = alloc_next_id((void ***)&pid_table, &pid_capacity, &next_pid);
        if (pid < 0) {
            spin_unlock_irq(&pid_lock, flags);
            slab_free(&proc_slab, p);
            return 0;
        }
        p->pid = (uint32_t)pid;
        pid_table[pid] = p;
        spin_unlock_irq(&pid_lock, flags);
        p->state = PROC_ALIVE;

        /* Single-user kernel: start with full capability set. Subsequent
         * capset() calls can narrow the effective/permitted bits. */
        p->cap_effective = CAP_FULL_SET;
        p->cap_permitted = CAP_FULL_SET;
        p->cap_inheritable = CAP_FULL_SET;
        p->cap_bounding   = CAP_FULL_SET;

        /* Default time namespace — init_time_ns (pinned, frozen).
         * clone()/do_fork overrides via time_ns_for_children inheritance. */
        p->time_ns              = time_ns_get(&init_time_ns);
        p->time_ns_for_children = time_ns_get(&init_time_ns);

        /* Default network namespace — init_net_ns (pinned). clone() with
         * CLONE_NEWNET swaps to a fresh NS in process_fork. */
        p->net_ns = net_ns_get(&init_net_ns);

        init_waitqueue_head(&p->children_wq);

        /* RLIMIT defaults: unlimited via RLIM_INFINITY sentinel. */
        p->rlim_data     = (unsigned long)~0UL;
        p->rlim_nproc    = (unsigned long)~0UL;
        p->rlim_fsize    = (unsigned long)~0UL;
        p->rlim_cpu_soft = (unsigned long)~0UL;
        p->rlim_cpu_hard = (unsigned long)~0UL;
    }
    return p;
}

int cred_has_cap_sys_admin(process_t *p) {
    if (!p) return 0;
    return (p->cap_effective & ((uint64_t)1 << CAP_SYS_ADMIN)) != 0;
}

/* ── Page table management ──────────────────────── */

uint64_t *alloc_page(void) {
    return (uint64_t *)page_alloc();
}

/* Convert PROT_* flags to x86 PTE flags (leaf entry only) */
uint64_t prot_to_pte(int prot) {
    /* PROT_NONE → not present (no access at all) */
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

/* Create user PML4: lower half empty (user), upper half = kernel direct map */
uint64_t *create_user_pml4(void) {
    uint64_t *user_pml4 = alloc_page();
    if (!user_pml4) return 0;
    /* Lower half: empty (user space) — alloc_page returns zeroed page */
    /* Upper half: share kernel mappings (direct physical map, no PTE_USER) */
    for (int i = 256; i < 512; i++)
        user_pml4[i] = pml4[i];
    return user_pml4;
}

/* Get or allocate a page table level for user mappings.
 * PTE entries store physical addresses; we convert via phys_to_virt/virt_to_phys. */
uint64_t *get_or_alloc_level(uint64_t *table, int idx) {
    if (table[idx] & PTE_PRESENT)
        return (uint64_t *)phys_to_virt(table[idx] & PTE_ADDR_MASK);

    uint64_t *new_tbl = alloc_page();
    if (!new_tbl) return 0;
    table[idx] = virt_to_phys(new_tbl) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return new_tbl;
}

int map_user_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot) {
    /* User pages only in lower half */
    if (vaddr >= 0x800000000000ULL) return -1;

    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    int pt_idx   = (vaddr >> 12) & 0x1FF;

    /* Intermediate levels always need WRITE+USER so the CPU can walk them */
    uint64_t *pdpt = get_or_alloc_level(user_pml4, pml4_idx);
    if (!pdpt) return -1;

    uint64_t *pd = get_or_alloc_level(pdpt, pdpt_idx);
    if (!pd) return -1;

    /* If PD entry is a 2MB huge page, split it into 512 × 4KB PTEs first */
    if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_PS)) {
        uint64_t pmd = pd[pd_idx];
        uint64_t huge_phys = pmd & 0x000FFFFFFFE00000ULL;
        uint64_t flags = pmd & (PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | PTE_COW);
        uint64_t *pt_new = alloc_page();
        if (!pt_new) return -1;
        for (int i = 0; i < 512; i++)
            pt_new[i] = (huge_phys + (uint64_t)i * 4096) | flags;
        pd[pd_idx] = virt_to_phys(pt_new) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    uint64_t *pt = get_or_alloc_level(pd, pd_idx);
    if (!pt) return -1;

    /* Only the leaf PTE restricts access based on prot */
    pt[pt_idx] = phys | prot_to_pte(prot);
    return 0;
}

/* OR additional bits into an existing leaf PTE.
 * Used by demand paging to set PTE_COW after map_user_page installs the
 * base PTE — keeps map_user_page's prot→PTE translation as the single
 * source of truth without duplicating the level walk. No-op if the leaf
 * is missing or huge. */
void pte_or_inplace(uint64_t *user_pml4, uint64_t vaddr, uint64_t bits) {
    if (vaddr >= 0x800000000000ULL) return;
    int pml4i = (vaddr >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
    int pdpti = (vaddr >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (vaddr >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT) || (pd[pdi] & PTE_PS)) return;
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    int pti = (vaddr >> 12) & 0x1FF;
    pt[pti] |= bits;
}

int map_user_huge_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot) {
    if (vaddr >= 0x800000000000ULL) return -1;
    if (vaddr & (0x200000ULL - 1)) return -1;  /* must be 2MB-aligned */
    if (phys  & (0x200000ULL - 1)) return -1;

    uint64_t *pdpt = get_or_alloc_level(user_pml4, (vaddr >> 39) & 0x1FF);
    if (!pdpt) return -1;
    uint64_t *pd = get_or_alloc_level(pdpt, (vaddr >> 30) & 0x1FF);
    if (!pd) return -1;

    int pd_idx = (vaddr >> 21) & 0x1FF;
    /* PMD entry with PS bit — direct 2MB mapping, no PT */
    uint64_t flags = PTE_PS | PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    pd[pd_idx] = phys | flags;
    return 0;
}

/* ── FD table init ──────────────────────────────── */

/* Default PTY for init's stdio. Set by vt_init(). */
int fd_default_pty = -1;

static int fd_bitmap_pages(int slots) {
    size_t words = slots / 64;
    if (words == 0) words = 1;
    size_t bytes = words * sizeof(uint64_t);
    return (int)((bytes + 4095) / 4096);
}

/* Power-of-two pages that hold leaf-pointer slots. */
static int fd_dir_pages(int leaf_slots) {
    size_t bytes = (size_t)leaf_slots * sizeof(fd_entry_t *);
    int n = (int)((bytes + 4095) / 4096);
    if (n < 1) n = 1;
    int p2 = 1;
    while (p2 < n) p2 *= 2;
    return p2;
}

/* Round up to a multiple of FD_LEAF_PER_PAGE so leaf math stays integer. */
static int fd_round_slots(int s) {
    int leaves = (s + FD_LEAF_PER_PAGE - 1) / FD_LEAF_PER_PAGE;
    return leaves * FD_LEAF_PER_PAGE;
}

/* Grow entries_dir to hold at least need_leaves slot-pointers. */
static int fd_dir_grow(fd_table_t *fdt, int need_leaves) {
    if (need_leaves <= fdt->dir_capacity) return 0;
    int new_cap = fdt->dir_capacity ? fdt->dir_capacity : 1;
    while (new_cap < need_leaves) new_cap *= 2;
    int new_pages = fd_dir_pages(new_cap);
    fd_entry_t **nd = (fd_entry_t **)pages_alloc(new_pages);
    if (!nd) return -ENOMEM;
    if (fdt->entries_dir) {
        for (int i = 0; i < fdt->dir_capacity; i++) nd[i] = fdt->entries_dir[i];
        pages_free(fdt->entries_dir, fd_dir_pages(fdt->dir_capacity));
    }
    fdt->entries_dir = nd;
    fdt->dir_capacity = new_cap;
    return 0;
}

/* The bitmap is sized to cover round_up(max_slots, 64) bits — see the
 * round-up dance below. Bits past max_slots are pre-marked as USED so
 * fd_find_free never hands out an out-of-bounds index. */
static int fd_bitmap_alloc_slots(int slots) { return (slots + 63) & ~63; }

/* Grow the flat free-bitmap to cover new_slots entries. The bitmap's
 * physical capacity is rounded to multiples of 64 so the word loop in
 * fd_find_free is well-defined; max_slots itself stays at the leaf-aligned
 * value the caller asked for. */
static int fd_bitmap_grow(fd_table_t *fdt, int new_slots) {
    if (new_slots <= fdt->max_slots) return 0;
    int alloc_slots = fd_bitmap_alloc_slots(new_slots);
    int old_alloc   = fd_bitmap_alloc_slots(fdt->max_slots);
    int new_words = alloc_slots / 64;
    uint64_t *nb = (uint64_t *)pages_alloc(fd_bitmap_pages(alloc_slots));
    if (!nb) return -ENOMEM;
    for (int w = 0; w < new_words; w++) nb[w] = ~0ULL;  /* all free */
    if (fdt->free_bitmap) {
        /* Carry over previously-set bits, but only inside the previously-
         * valid range [0, max_slots). The old slack [max_slots, old_alloc)
         * had been masked USED — re-init clean and let the new slack
         * masking below place USED bits correctly for the new size. */
        for (int b = 0; b < fdt->max_slots; b++) {
            uint64_t bit = 1ULL << (b % 64);
            if (!(fdt->free_bitmap[b / 64] & bit))
                nb[b / 64] &= ~bit;
        }
        pages_free(fdt->free_bitmap, fd_bitmap_pages(old_alloc));
    }
    fdt->free_bitmap = nb;
    fdt->max_slots   = new_slots;
    /* Mark the slack bits (max_slots..alloc_slots-1) as USED so we never
     * hand out an fd > max_slots-1 even though the bitmap word covers it. */
    for (int b = new_slots; b < alloc_slots; b++)
        fdt->free_bitmap[b / 64] &= ~(1ULL << (b % 64));
    return 0;
}

/* Lazy leaf allocation. fd must be < fdt->max_slots when called. */
static int fd_leaf_alloc(fd_table_t *fdt, int fd) {
    int li = fd / FD_LEAF_PER_PAGE;
    if (fdt->entries_dir[li]) return 0;
    fd_entry_t *page = (fd_entry_t *)pages_alloc(1);
    if (!page) return -ENOMEM;
    fdt->entries_dir[li] = page;
    fdt->leaf_count++;
    return 0;
}

/* Public wrapper for callers in other TUs (currently fork's dup_fd_table). */
int fd_table_leaf_ensure(fd_table_t *fdt, int fd) {
    return fd_leaf_alloc(fdt, fd);
}

/* fd_alloc/expand/dup may be entered concurrently via signal-path syscalls;
 * all FD mutations already run under the process syscall context, but the
 * expansion itself must be atomic w.r.t. interrupts. Table grow is rare. */
static int fd_table_grow(fd_table_t *fdt, int min_slots) {
    if (min_slots > FD_CEILING) return -EMFILE;
    int new_slots = fdt->max_slots ? fdt->max_slots : FD_INIT_SLOTS;
    while (new_slots < min_slots) new_slots *= 2;
    if (new_slots > FD_CEILING) new_slots = FD_CEILING;
    if (new_slots < min_slots) return -EMFILE;
    new_slots = fd_round_slots(new_slots);
    if (new_slots > FD_CEILING) new_slots = FD_CEILING;

    int need_leaves = (new_slots + FD_LEAF_PER_PAGE - 1) / FD_LEAF_PER_PAGE;
    int r = fd_dir_grow(fdt, need_leaves);
    if (r) return r;
    return fd_bitmap_grow(fdt, new_slots);
}

int fd_table_alloc_empty(fd_table_t *fdt, int slots) {
    fdt->entries_dir = 0;
    fdt->dir_capacity = 0;
    fdt->leaf_count = 0;
    fdt->free_bitmap = 0;
    fdt->max_slots = 0;
    fdt->max_fd = 0;
    if (slots < FD_INIT_SLOTS) slots = FD_INIT_SLOTS;
    return fd_table_grow(fdt, slots);
}

int fd_table_init(fd_table_t *fdt) {
    fdt->entries_dir = 0;
    fdt->dir_capacity = 0;
    fdt->leaf_count = 0;
    fdt->free_bitmap = 0;
    fdt->max_slots = 0;
    fdt->max_fd = 0;
    if (fd_table_grow(fdt, FD_INIT_SLOTS) < 0) return -ENOMEM;
    if (fd_leaf_alloc(fdt, 0) < 0) return -ENOMEM;

    fd_entry_t *p0 = fd_entry_at(fdt, 0);
    fd_entry_t *p1 = fd_entry_at(fdt, 1);
    fd_entry_t *p2 = fd_entry_at(fdt, 2);
    if (fd_default_pty >= 0) {
        void *pty = (void *)(uintptr_t)fd_default_pty;
        *p0 = (fd_entry_t){FD_PTY_SLAVE, pty, O_RDONLY};
        *p1 = (fd_entry_t){FD_PTY_SLAVE, pty, O_WRONLY};
        *p2 = (fd_entry_t){FD_PTY_SLAVE, pty, O_WRONLY};
    } else {
        *p0 = (fd_entry_t){FD_SERIAL, 0, O_RDONLY};
        *p1 = (fd_entry_t){FD_SERIAL, 0, O_WRONLY};
        *p2 = (fd_entry_t){FD_SERIAL, 0, O_WRONLY};
    }
    fd_mark_used(fdt, 0);
    fd_mark_used(fdt, 1);
    fd_mark_used(fdt, 2);
    fdt->max_fd = 3;
    return 0;
}

void fd_table_free(fd_table_t *fdt) {
    if (fdt->entries_dir) {
        for (int i = 0; i < fdt->dir_capacity; i++) {
            if (fdt->entries_dir[i]) pages_free(fdt->entries_dir[i], 1);
        }
        pages_free(fdt->entries_dir, fd_dir_pages(fdt->dir_capacity));
    }
    if (fdt->free_bitmap)
        pages_free(fdt->free_bitmap,
                   fd_bitmap_pages(fd_bitmap_alloc_slots(fdt->max_slots)));
    fdt->entries_dir = 0;
    fdt->dir_capacity = 0;
    fdt->leaf_count = 0;
    fdt->free_bitmap = 0;
    fdt->max_slots = 0;
    fdt->max_fd = 0;
}

/* Effective RLIMIT_NOFILE: 0 = unset → FD_DEFAULT_NOFILE (Linux ulimit -n). */
static unsigned long fd_effective_nofile(process_t *p) {
    if (!p) return FD_DEFAULT_NOFILE;
    unsigned long n = p->rlim_nofile;
    return n ? n : FD_DEFAULT_NOFILE;
}

/* Ensure capacity for at least `min_slots` entries, bounded by nofile.
 * Returns 0 on success, -EMFILE if nofile cap reached, -ENOMEM on OOM.
 * Callers may pass an aggressive want (e.g. max_slots*2) — we clamp to
 * the effective cap so doubling past the rlimit doesn't reject the
 * still-legal smaller request. */
static int fd_ensure_capacity(fd_table_t *fdt, int min_slots, unsigned long nofile) {
    if (min_slots <= fdt->max_slots) return 0;
    unsigned long cap = nofile < (unsigned long)FD_CEILING ? nofile : (unsigned long)FD_CEILING;
    if ((unsigned long)min_slots > cap) min_slots = (int)cap;
    if (min_slots <= fdt->max_slots) return -EMFILE;
    return fd_table_grow(fdt, min_slots);
}

int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags) {
    process_t *p = proc_current();
    unsigned long nofile = fd_effective_nofile(p);

    int fd = fd_find_free(fdt, 0);
    if (fd < 0 || (unsigned long)fd >= nofile) {
        int want = fdt->max_slots ? fdt->max_slots + 1 : FD_INIT_SLOTS;
        int r = fd_ensure_capacity(fdt, want, nofile);
        if (r < 0) return r;
        fd = fd_find_free(fdt, 0);
        if (fd < 0 || (unsigned long)fd >= nofile) return -EMFILE;
    }
    if (fd_leaf_alloc(fdt, fd) < 0) return -ENOMEM;
    *fd_entry_at(fdt, fd) = (fd_entry_t){type, obj, flags};
    fd_mark_used(fdt, fd);
    if (fd >= fdt->max_fd) fdt->max_fd = fd + 1;
    return fd;
}

int fd_dup_at(fd_table_t *fdt, int minfd, fd_entry_t src, int new_flags) {
    process_t *p = proc_current();
    unsigned long nofile = fd_effective_nofile(p);
    if (minfd < 0) return -EINVAL;
    if ((unsigned long)minfd >= nofile) return -EMFILE;

    int fd = fd_find_free(fdt, minfd);
    if (fd < 0 || (unsigned long)fd >= nofile) {
        int want = fdt->max_slots * 2;
        if (want <= minfd) want = minfd + 1;
        int r = fd_ensure_capacity(fdt, want, nofile);
        if (r < 0) return r;
        fd = fd_find_free(fdt, minfd);
        if (fd < 0 || (unsigned long)fd >= nofile) return -EMFILE;
    }
    if (fd_leaf_alloc(fdt, fd) < 0) return -ENOMEM;
    src.flags = new_flags;
    *fd_entry_at(fdt, fd) = src;
    fd_mark_used(fdt, fd);
    if (fd >= fdt->max_fd) fdt->max_fd = fd + 1;
    return fd;
}

int fd_install_at(fd_table_t *fdt, int newfd, fd_entry_t src) {
    process_t *p = proc_current();
    unsigned long nofile = fd_effective_nofile(p);
    if (newfd < 0) return -EBADF;
    if ((unsigned long)newfd >= nofile) return -EMFILE;
    if (newfd >= fdt->max_slots) {
        int r = fd_ensure_capacity(fdt, newfd + 1, nofile);
        if (r < 0) return r;
    }
    if (fd_leaf_alloc(fdt, newfd) < 0) return -ENOMEM;
    *fd_entry_at(fdt, newfd) = src;
    fd_mark_used(fdt, newfd);
    if (newfd >= fdt->max_fd) fdt->max_fd = newfd + 1;
    return 0;
}

int fd_close(fd_table_t *fdt, int fd) {
    if (fd < 0 || fd >= fdt->max_slots) return -1;
    fd_entry_t *e = fd_entry_at(fdt, fd);
    if (!e || e->type == FD_NONE) return -1;
    e->type = FD_NONE;
    e->obj = 0;
    fd_mark_free(fdt, fd);
    return 0;
}

fd_entry_t *fd_get(fd_table_t *fdt, int fd) {
    if (fd < 0 || fd >= fdt->max_slots) return 0;
    fd_entry_t *e = fd_entry_at(fdt, fd);
    if (!e || e->type == FD_NONE) return 0;
    return e;
}

/* ── ASLR ───────────────────────────────────────── */

/* ASLR via kernel CSPRNG, fallback to RDTSC before init */
uint64_t aslr_rand(void) {
    uint64_t r;
    extern int random_get(void *buf, size_t len);
    if (random_get(&r, sizeof(r)) == 0) return r;
    return hal_cpu_timestamp();
}

/* ── Process creation ───────────────────────────── */

int proc_create_elf(const void *elf_data, size_t elf_len) {
    process_t *p = proc_alloc();
    if (!p) return -1;

    p->pml4 = create_user_pml4();
    if (!p->pml4) goto fail_slab;
    p->vma_root = 0;

    /* vDSO: must be mapped before build_user_stack writes AT_SYSINFO_EHDR.
     * Pass the target process' time_ns (here always init at proc_alloc time)
     * so vdso_map honours per-NS offsets uniformly across exec/fork callers. */
    extern int vdso_map(uint64_t *user_pml4, vma_t **vma_root,
                        struct time_namespace *target_ns);
    vdso_map(p->pml4, &p->vma_root, p->time_ns);

    /* ASLR: randomize stack and mmap base */
    uint64_t stack_rand = aslr_rand() & 0x3FFFFF000ULL; /* 22-bit, 4KB aligned */
    uint64_t mmap_rand  = aslr_rand() & 0xFFFFFFF000ULL;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    p->mmap_next = USER_MMAP_BASE - mmap_rand;

    /* Load ELF with proper argv/envp for init */
    const char *init_argv[] = { "/init" };
    const char *init_envp[] = { "HOME=/", "PATH=/bin:/usr/bin", "TERM=linux" };
    uint64_t entry, stack_ptr, brk_end;
    if (elf_load(elf_data, elf_len, p->pml4, stack_top,
                 init_argv, 1, init_envp, 3,
                 &entry, &stack_ptr, &brk_end) < 0)
        goto fail_pml4;

    p->brk_base = brk_end;
    p->brk_current = brk_end;
    p->is_driver = (p->pid == 1) ? 1 : 0;
    p->notify_signal = SIGCHLD;
    p->umask_val = 0022;
    p->pgid = p->pid;

    p->sid  = p->pid;  /* initial process is own session leader */
    p->cwd[0] = '/'; p->cwd[1] = '\0';
    { const char *s = "/init"; int ii = 0; while (s[ii] && ii < 255) { p->exe_path[ii] = s[ii]; ii++; } p->exe_path[ii] = 0; }
    { const char *s = "init";  int ii = 0; while (s[ii] && ii < 15)  { p->comm[ii] = s[ii]; ii++; } p->comm[ii] = 0; }
    { const char *s = "/init"; int ii = 0; while (s[ii] && ii < 1023) { p->cmdline[ii] = s[ii]; ii++; } p->cmdline[ii] = 0; p->cmdline_len = ii + 1; }
    if (fd_table_init(&p->fds) < 0) goto fail_pml4;

    /* Stack: small initial VMA with VMA_GROWSDOWN (expands on page fault).
     * Like Linux: initial 132KB, grows to RLIMIT_STACK on demand.
     * PROT_NONE guard VMA at the growth limit makes overflow fail-stop. */
    uint64_t stack_bottom = stack_top - USER_STACK_INIT;
    uint64_t stack_floor  = stack_top - RLIM_STACK_DEFAULT;
    vma_insert(&p->vma_root, stack_bottom, stack_top,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | VMA_GROWSDOWN);
    vma_insert(&p->vma_root, stack_floor - STACK_GUARD_SIZE, stack_floor,
               0, MAP_PRIVATE | MAP_ANONYMOUS);
    p->stack_top = stack_top;

    /* brk VMA is created on first brk() call that grows beyond brk_base.
     * Don't insert a zero-length VMA here — it corrupts the AVL tree. */

    /* Create VMAs for ELF segments by scanning the ELF headers */
    if (elf_len >= 64) {
        const uint8_t *data = (const uint8_t *)elf_data;
        uint64_t phoff = *(const uint64_t *)(data + 32);
        uint16_t phentsize = *(const uint16_t *)(data + 54);
        uint16_t phnum = *(const uint16_t *)(data + 56);
        if (phoff + (uint64_t)phnum * phentsize > elf_len)
            goto fail_elf;

        for (int i = 0; i < phnum; i++) {
            const uint8_t *ph = data + phoff + (uint64_t)i * phentsize;
            uint32_t p_type = *(const uint32_t *)ph;
            if (p_type != 1) continue;
            uint64_t p_vaddr = *(const uint64_t *)(ph + 16);
            uint64_t p_memsz = *(const uint64_t *)(ph + 40);
            uint32_t p_flags = *(const uint32_t *)(ph + 4);
            if (p_memsz == 0) continue;
            uint64_t seg_start = p_vaddr & ~0xFFFULL;
            uint64_t seg_end = (p_vaddr + p_memsz + 0xFFF) & ~0xFFFULL;
            int prot = 0;
            if (p_flags & 4) prot |= PROT_READ;
            if (p_flags & 2) prot |= PROT_WRITE;
            if (p_flags & 1) prot |= PROT_EXEC;
            vma_insert(&p->vma_root, seg_start, seg_end, prot, MAP_PRIVATE);
        }
    }

    /* Create main thread */
    thread_t *t = thread_alloc();
    if (!t) goto fail_elf;

    t->state = THREAD_RUNNABLE;
    t->rip = entry;
    t->rsp = stack_ptr;
    t->rflags = 0x202;
    t->sched_policy = SCHED_OTHER;
    t->priority = PRIO_MIN;
    t->saved_priority = -1;
    t->fs_base = 0;
    t->gs_base = 0;
    t->cpu_affinity = -1;
    t->timeslice = RR_TIMESLICE;
    t->proc = p;

    /* Initialize FPU/SSE/AVX state: clean XSAVE/FXSAVE image.
     * FCW=0x037F: extended precision, all x87 exceptions masked.
     * MXCSR=0x1F80: all SSE exceptions masked, round-to-nearest.
     * XSTATE_BV at offset 512 marks which components are valid. */
    *(uint16_t *)(t->xsave_area + 0) = 0x037F;   /* FCW */
    *(uint32_t *)(t->xsave_area + 24) = 0x1F80;  /* MXCSR */
    if (xsave_size > 512)
        *(uint64_t *)(t->xsave_area + 512) = xsave_xcr0; /* XSTATE_BV */

    /* Kernel stack for this thread */
    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) goto fail_thread;
    t->kstack_top = (uint64_t)(uintptr_t)(t->kstack + KSTACK_SIZE);
    thread_init_kstack(t);

    p->main_thread = t;
    p->threads = t;
    p->thread_count = 1;

    /* Add to scheduler */
    extern void sched_add(thread_t *t);
    sched_add(t);

    return (int)p->pid;

fail_thread:
    thread_free(t);
fail_elf:
    free_address_space(p->pml4);
    p->pml4 = 0;
    vma_free_tree(p->vma_root);
    p->vma_root = 0;
    slab_free(&proc_slab, p);
    return -1;
fail_pml4:
    free_address_space(p->pml4);
    p->pml4 = 0;
    slab_free(&proc_slab, p);
    return -1;
fail_slab:
    slab_free(&proc_slab, p);
    return -1;
}

/* Create process from a VFS file path (reads file into memory, loads as ELF).
 * Used for userspace drivers like e1000d that are registered in /bin/. */
int proc_create_from_vfs(const char *path) {
    extern int vfs_read_file(const char *, uint8_t **, size_t *);
    uint8_t *data = 0;
    size_t size = 0;
    if (vfs_read_file(path, &data, &size) < 0 || !data) return -1;

    int pid = proc_create_elf(data, size);

    /* Mark as driver (gets HW access permissions) */
    if (pid > 0) {
        process_t *p = proc_find((uint32_t)pid);
        if (p) p->is_driver = 1;
    }

    int npages = (int)((size + 4095) / 4096);
    pages_free(data, npages);
    return pid;
}

/* ── Context switching ──────────────────────────── */

extern void proc_enter_ring3(thread_t *t) __attribute__((noreturn));

/* ret_from_fork (assembly in context.asm): entered via context_switch's ret.
 * RSP points to the fake syscall frame. Sets percpu->user_rsp and
 * percpu->syscall_frame, loads RAX from saved frame, jumps to syscall_return. */
extern void ret_from_fork(void);

/* Set up a new thread's kstack so context_switch "returns" into ret_from_fork,
 * which then falls through to the syscall return path (sysret to userspace).
 *
 * Stack layout (growing down from kstack_top):
 *   ┌─ syscall_entry_asm compatible frame (15 regs) ─┐
 *   │  r15, r14, r13, r12, rbp, rbx, r9, r8,         │
 *   │  r10, rdx, rsi, rdi, rax, r11(RFLAGS), rcx(RIP) │
 *   └──────────────────────────────────────────────────┘
 *   ┌─ context_switch frame ─────────────────────────┐
 *   │  ret_from_fork, RFLAGS, rbp, rbx, r12-r15      │
 *   └──────────────────────────────────────────────────┘
 *   ↑ kstack_rsp points here
 */
void thread_init_kstack(thread_t *t) {
    uint64_t *sp = (uint64_t *)(t->kstack + KSTACK_SIZE);

    /* Syscall frame (matches syscall_entry_asm push order, read by sysret path).
     * Pushed in reverse order of syscall_entry_asm:
     *   push rcx(=RIP), push r11(=RFLAGS), push rax, push rdi, push rsi,
     *   push rdx, push r10, push r8, push r9, push rbx, push rbp,
     *   push r12, push r13, push r14, push r15 */
    *--sp = t->rip;    /* rcx = user RIP (sysret loads RCX → RIP) */
    *--sp = t->rflags & ~0x100ULL; /* r11 = user RFLAGS, clear TF (trap flag) */
    *--sp = t->rax;    /* rax (return value: 0 for child, pid for parent) */
    *--sp = t->rdi;    /* rdi */
    *--sp = t->rsi;    /* rsi */
    *--sp = t->rdx;    /* rdx */
    *--sp = t->r10;    /* r10 */
    *--sp = t->r8;     /* r8 */
    *--sp = t->r9;     /* r9 */
    *--sp = t->rbx;    /* rbx */
    *--sp = t->rbp;    /* rbp */
    *--sp = t->r12;    /* r12 */
    *--sp = t->r13;    /* r13 */
    *--sp = t->r14;    /* r14 */
    *--sp = t->r15;    /* r15 */

    /* context_switch frame: pushfq, push rbp..r15, then ret addr */
    *--sp = (uint64_t)ret_from_fork; /* ret addr */
    *--sp = 0x002; /* RFLAGS: bit 1 set, IF=0 */
    *--sp = 0;     /* rbp */
    *--sp = 0;     /* rbx */
    *--sp = 0;     /* r12 */
    *--sp = 0;     /* r13 */
    *--sp = 0;     /* r14 */
    *--sp = 0;     /* r15 */
    t->kstack_rsp = (uint64_t)sp;
}

process_t *proc_current(void) {
    thread_t *t = thread_current();
    return t ? t->proc : 0;
}

void proc_yield(void) {
    /* no-op in kernel context */
}

/* ── Process lookup — O(1) via pid_table ─────────── */

process_t *proc_find(uint32_t pid) {
    if (pid == 0 || pid >= (uint32_t)pid_capacity) return 0;
    process_t *p = pid_table[pid];
    if (p && __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != PROC_FREE && p->pid == pid)
        return p;
    return 0;
}

/* ── Thread lookup — O(1) via tid_table ──────────── */

thread_t *thread_find_by_tid(int tid) {
    if (tid <= 0 || tid >= tid_capacity) return 0;
    thread_t *t = tid_table[tid];
    if (t && t->state != THREAD_FREE && t->state != THREAD_DEAD && t->tid == tid)
        return t;
    return 0;
}

/* ── Credential helpers ─────────────────────────── */

/* Round bytes up to whole 4KB pages, then up to the next power-of-two so
 * pages_alloc/pages_free see matching counts (buddy allocator requirement). */
static int cred_groups_pages_for(int count) {
    if (count <= 0) return 0;
    size_t bytes = (size_t)count * sizeof(uint32_t);
    int pages = (int)((bytes + 4095) / 4096);
    int p2 = 1;
    while (p2 < pages) p2 *= 2;
    return p2;
}

void cred_groups_free(process_t *p) {
    if (!p || !p->groups) return;
    pages_free(p->groups, p->groups_pages);
    p->groups = 0;
    p->groups_pages = 0;
    p->ngroups = 0;
}

/* Replace the supplementary GID array. count==0 frees the backing storage.
 * Caller has validated the input GIDs (already kernel-resident copy). */
int cred_groups_set(process_t *p, const uint32_t *list, int count) {
    if (!p) return -EFAULT;
    if (count < 0 || count > NGROUPS_MAX) return -EINVAL;
    if (count == 0) { cred_groups_free(p); return 0; }
    int need_pages = cred_groups_pages_for(count);
    if (need_pages != p->groups_pages) {
        uint32_t *fresh = (uint32_t *)pages_alloc(need_pages);
        if (!fresh) return -ENOMEM;
        if (p->groups) pages_free(p->groups, p->groups_pages);
        p->groups = fresh;
        p->groups_pages = need_pages;
    }
    for (int i = 0; i < count; i++) p->groups[i] = list[i];
    p->ngroups = count;
    return 0;
}

int cred_in_group(process_t *p, uint32_t gid) {
    if (!p) return 0;
    if (p->egid == gid) return 1;
    for (int i = 0; i < p->ngroups; i++)
        if (p->groups[i] == gid) return 1;
    return 0;
}

int cred_owns(process_t *p, uint32_t owner_uid) {
    return p && p->euid == owner_uid;
}

int cred_can_chmod(process_t *p, uint32_t owner_uid) {
    if (!p) return 1;  /* kernel context: allow */
    if (p->euid == 0) return 1;
    return p->euid == owner_uid;
}

int cred_can_chown_gid(process_t *p, uint32_t owner_uid,
                       uint32_t target_gid, int gid_specified) {
    if (!p) return 0;
    if (p->euid == 0) return 1;
    if (p->euid != owner_uid) return 0;
    if (!gid_specified) return 1;
    return cred_in_group(p, target_gid);
}

int cred_may_access(process_t *p, uint32_t owner_uid, uint32_t owner_gid,
                    uint32_t mode, int want) {
    if (!p || p->euid == 0) return 0;
    uint32_t perms;
    if (p->euid == owner_uid) {
        perms = (mode >> 6) & 7;
    } else if (cred_in_group(p, owner_gid)) {
        perms = (mode >> 3) & 7;
    } else {
        perms = mode & 7;
    }
    if ((want & perms) != (uint32_t)want) return -EACCES;
    return 0;
}

/* ── Address space helpers ───────────────────────── */

/* Read PTE for a user virtual address. Returns 0 if not mapped.
 * For 2MB huge pages (PS bit in PMD): returns a synthetic PTE with
 * the correct sub-page physical address and PTE_PS set. */
uint64_t read_pte_pub(uint64_t *user_pml4, uint64_t va) {
    int pml4i = (va >> 39) & 0x1FF;
    if (!(user_pml4[pml4i] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
    int pdpti = (va >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
    int pdi = (va >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    /* Huge page: PMD entry directly maps 2MB */
    if (pd[pdi] & PTE_PS) {
        uint64_t huge_phys = pd[pdi] & 0x000FFFFFFFE00000ULL;
        uint64_t offset = va & 0x1FFFFFULL;
        uint64_t flags = pd[pdi] & ~0x000FFFFFFFE00000ULL;
        return (huge_phys + offset) | (flags & ~PTE_PS);
    }
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
    int pti = (va >> 12) & 0x1FF;
    return pt[pti];
}
