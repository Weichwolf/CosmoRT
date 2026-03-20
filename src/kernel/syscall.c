/* CosmoRT POSIX Syscall Layer — thread-aware, SMP-safe */

#include "syscall.h"
#include "process.h"
#include "percpu.h"
#include "serial.h"
#include "config.h"
#include "vma.h"
#include "page_alloc.h"
#include "futex.h"
#include "timer.h"

/* PTE flags */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_NX      (1ULL << 63)

static void serial_hex64(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4)
        serial_putchar("0123456789abcdef"[(v >> i) & 0xf]);
}

/* ── SYS_write (1) ───────────────────────────────── */

static long do_write(int fd, const void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_SERIAL) {
        const char *s = (const char *)buf;
        for (size_t i = 0; i < count && i < 0x10000; i++)
            serial_putchar(s[i]);
        return (long)count;
    }
    return -EBADF;
}

/* ── SYS_writev (20) ────────────────────────────── */

struct iovec { const void *iov_base; size_t iov_len; };

static long do_writev(int fd, const struct iovec *iov, int iovcnt) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_SERIAL) return -EBADF;
    long total = 0;
    for (int i = 0; i < iovcnt && i < 1024; i++) {
        const char *s = (const char *)iov[i].iov_base;
        for (size_t j = 0; j < iov[i].iov_len && j < 0x10000; j++)
            serial_putchar(s[j]);
        total += (long)iov[i].iov_len;
    }
    return total;
}

/* ── SYS_read (0) ────────────────────────────────── */

static long do_read(int fd, void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_SERIAL) {
        extern char serial_getchar(void);
        char *dst = (char *)buf;
        for (size_t i = 0; i < count; i++) {
            char c = serial_getchar();
            if (c == 0) return (long)i; /* no more data */
            dst[i] = c;
        }
        return (long)count;
    }
    return -EBADF;
}

/* ── SYS_brk (12) ───────────────────────────────── */

static long do_brk(unsigned long addr) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (addr == 0) return (long)p->brk_current;
    if (addr < p->brk_base) return (long)p->brk_current;

    uint64_t old_end = (p->brk_current + 0xFFF) & ~0xFFFULL;
    uint64_t new_end = (addr + 0xFFF) & ~0xFFFULL;

    if (new_end > old_end) {
        /* Growing: pages allocated on demand (page fault handler) */
    } else if (new_end < old_end) {
        /* Shrinking: unmap pages */
        for (uint64_t va = new_end; va < old_end; va += 4096) {
            /* Walk page tables to find and free the physical page */
            /* For now, just clear the PTE — page leak is acceptable */
        }
    }

    /* Update brk VMA */
    vma_t *brk_vma = vma_find(p->vma_root, p->brk_base);
    if (brk_vma && brk_vma->start == p->brk_base) {
        brk_vma->end = new_end;
    } else if (new_end > p->brk_base) {
        vma_insert(&p->vma_root, p->brk_base, new_end,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    }

    p->brk_current = addr;
    return (long)addr;
}

/* ── Pre-fault helper: allocate + map all pages in range ── */

static void prefault_range(uint64_t *user_pml4, uint64_t start, uint64_t end, int prot) {
    (void)prot;
    for (uint64_t va = start; va < end; va += 4096) {
        /* Check if already mapped */
        int pml4i = (va >> 39) & 0x1FF;
        int mapped = 0;
        if (user_pml4[pml4i] & PTE_PRESENT) {
            uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & ~0xFFFULL);
            int pdpti = (va >> 30) & 0x1FF;
            if (pdpt[pdpti] & PTE_PRESENT) {
                uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & ~0xFFFULL);
                int pdi = (va >> 21) & 0x1FF;
                if (pd[pdi] & PTE_PRESENT) {
                    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & ~0xFFFULL);
                    int pti = (va >> 12) & 0x1FF;
                    if (pt[pti] & PTE_PRESENT) mapped = 1;
                }
            }
        }
        if (!mapped) {
            uint64_t *page = alloc_page();
            if (page)
                map_user_page(user_pml4, va, virt_to_phys(page));
        }
    }
}

/* ── SYS_mlockall (151) / SYS_munlockall (152) ──── */

static void vma_walk_prefault(vma_t *node, uint64_t *pml4) {
    if (!node) return;
    vma_walk_prefault(node->left, pml4);
    prefault_range(pml4, node->start, node->end, node->prot);
    node->flags |= VMA_LOCKED;
    vma_walk_prefault(node->right, pml4);
}

static long do_mlockall(int flags) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (flags & ~(MCL_CURRENT | MCL_FUTURE)) return -EINVAL;
    p->mlockall_flags = flags;
    if (flags & MCL_CURRENT)
        vma_walk_prefault(p->vma_root, p->pml4);
    return 0;
}

static long do_munlockall(void) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    p->mlockall_flags = 0;
    return 0;
}

/* ── SYS_mlock (149) / SYS_munlock (150) ─────────── */

static long do_mlock(unsigned long addr, size_t len) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    addr &= ~0xFFFULL;
    len = (len + 0xFFF) & ~0xFFFULL;
    /* Find and lock VMAs in range */
    for (uint64_t va = addr; va < addr + len; ) {
        vma_t *v = vma_find(p->vma_root, va);
        if (!v) { va += 4096; continue; }
        v->flags |= VMA_LOCKED;
        uint64_t end = v->end < addr + len ? v->end : addr + len;
        prefault_range(p->pml4, va, end, v->prot);
        va = v->end;
    }
    return 0;
}

static long do_munlock(unsigned long addr, size_t len) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    addr &= ~0xFFFULL;
    len = (len + 0xFFF) & ~0xFFFULL;
    for (uint64_t va = addr; va < addr + len; ) {
        vma_t *v = vma_find(p->vma_root, va);
        if (!v) { va += 4096; continue; }
        v->flags &= ~VMA_LOCKED;
        va = v->end;
    }
    return 0;
}

/* ── SYS_mmap (9) ───────────────────────────────── */

static long do_mmap(unsigned long addr, size_t length, int prot,
                    int flags, int fd, long offset) {
    (void)offset;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (fd != -1 && !(flags & MAP_ANONYMOUS)) return -ENOSYS;

    length = (length + 0xFFF) & ~0xFFFULL;
    if (length == 0) return -EINVAL;

    uint64_t vaddr;
    if (addr && (flags & MAP_FIXED)) {
        vaddr = addr & ~0xFFFULL;
        /* Remove any overlapping VMAs in [vaddr, vaddr+length) */
        for (;;) {
            vma_t *ov = vma_find(p->vma_root, vaddr);
            if (!ov) {
                /* Also check for VMAs that start within our range */
                int found = 0;
                /* Scan: try midpoints */
                for (uint64_t probe = vaddr; probe < vaddr + length; probe += 4096) {
                    ov = vma_find(p->vma_root, probe);
                    if (ov) { found = 1; break; }
                }
                if (!found) break;
            }
            if (ov->start >= vaddr + length) break;
            /* Split/remove overlap */
            if (ov->start < vaddr && ov->end > vaddr + length) {
                /* VMA straddles both sides — split into two */
                uint64_t orig_end = ov->end;
                ov->end = vaddr;
                vma_insert(&p->vma_root, vaddr + length, orig_end,
                           ov->prot, ov->flags);
            } else if (ov->start < vaddr) {
                ov->end = vaddr;
            } else if (ov->end > vaddr + length) {
                ov->start = vaddr + length;
            } else {
                vma_remove(&p->vma_root, ov);
            }
            /* Check for more overlaps */
            ov = vma_find(p->vma_root, vaddr);
            if (!ov) break;
            if (ov->start >= vaddr + length) break;
        }
    } else {
        vaddr = vma_find_free(p->vma_root, p->mmap_next, length);
        if (!vaddr) return -ENOMEM;
        p->mmap_next = vaddr;
    }

    /* Create VMA — pages allocated on demand (page fault) unless locked */
    int vma_flags = flags;
    if (p->mlockall_flags & MCL_FUTURE) vma_flags |= VMA_LOCKED;
    vma_t *v = vma_insert(&p->vma_root, vaddr, vaddr + length, prot, vma_flags);
    if (!v) return -ENOMEM;

    /* If locked, pre-fault all pages now (no demand paging latency) */
    if (vma_flags & VMA_LOCKED)
        prefault_range(p->pml4, vaddr, vaddr + length, prot);

    return (long)vaddr;
}

/* ── Page table walk helpers for unmap/mprotect ──── */

static void unmap_range(uint64_t *user_pml4, uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & ~0xFFFULL);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & ~0xFFFULL);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & ~0xFFFULL);

        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & ~0xFFFULL;
            pt[pti] = 0;
            page_free(phys_to_virt(phys));
            /* TLB flush for this address */
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        }
    }
}

static uint64_t prot_to_pte_flags(int prot) {
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

static void update_pte_prot(uint64_t *user_pml4, uint64_t start, uint64_t end, int prot) {
    uint64_t new_flags = prot_to_pte_flags(prot);
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & ~0xFFFULL);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & ~0xFFFULL);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & ~0xFFFULL);

        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & ~0xFFFULL;
            pt[pti] = phys | new_flags;
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        }
    }
}

/* ── SYS_munmap (11) / SYS_mprotect (10) ────────── */

static long do_munmap(unsigned long addr, size_t length) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (addr & 0xFFF) return -EINVAL;

    length = (length + 0xFFF) & ~0xFFFULL;
    uint64_t start = addr;
    uint64_t end = addr + length;

    /* Unmap physical pages */
    unmap_range(p->pml4, start, end);

    /* Adjust VMAs: find and remove/split overlapping VMAs */
    for (;;) {
        vma_t *v = 0;
        /* Find any VMA overlapping [start, end) */
        for (uint64_t probe = start; probe < end; probe += 4096) {
            v = vma_find(p->vma_root, probe);
            if (v) break;
        }
        if (!v) break;
        if (v->start >= end || v->end <= start) break;

        if (v->start >= start && v->end <= end) {
            /* Entirely within unmap range */
            vma_remove(&p->vma_root, v);
        } else if (v->start < start && v->end > end) {
            /* VMA straddles both sides — split */
            uint64_t orig_end = v->end;
            v->end = start;
            vma_insert(&p->vma_root, end, orig_end, v->prot, v->flags);
            break;
        } else if (v->start < start) {
            v->end = start;
        } else {
            v->start = end;
        }
    }

    return 0;
}

static long do_mprotect(unsigned long addr, size_t len, int prot) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (addr & 0xFFF) return -EINVAL;

    len = (len + 0xFFF) & ~0xFFFULL;
    uint64_t start = addr;
    uint64_t end = addr + len;

    /* Update PTE permissions */
    update_pte_prot(p->pml4, start, end, prot);

    /* Update VMA prot flags, splitting if needed */
    for (uint64_t probe = start; probe < end; ) {
        vma_t *v = vma_find(p->vma_root, probe);
        if (!v) { probe += 4096; continue; }

        if (v->start < start) {
            /* Split: left part keeps old prot */
            uint64_t orig_end = v->end;
            int orig_prot = v->prot;
            int orig_flags = v->flags;
            v->end = start;
            vma_insert(&p->vma_root, start, orig_end, orig_prot, orig_flags);
            continue; /* re-probe */
        }
        if (v->end > end) {
            /* Split: right part keeps old prot */
            int orig_prot = v->prot;
            int orig_flags = v->flags;
            vma_insert(&p->vma_root, end, v->end, orig_prot, orig_flags);
            v->end = end;
        }
        v->prot = prot;
        probe = v->end;
    }

    return 0;
}

/* ── SYS_arch_prctl (158) ────────────────────────── */

static long do_arch_prctl(int code, unsigned long addr) {
    if (code == ARCH_SET_FS) {
        __asm__ volatile("wrmsr" :: "c"(0xC0000100),
                         "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
        return 0;
    }
    if (code == ARCH_SET_GS) {
        /* User GS: write to IA32_KERNEL_GS_BASE so next swapgs restores it.
         * No — that would clobber our percpu pointer!
         * User GS must be stored in thread context and restored on sysret. */
        /* For now: unsupported */
        return -EINVAL;
    }
    if (code == ARCH_GET_FS) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        *(unsigned long *)addr = ((uint64_t)hi << 32) | lo;
        return 0;
    }
    return -EINVAL;
}

/* ── SYS_exit / SYS_exit_group ───────────────────── */

static void do_exit(int status) {
    thread_t *t = thread_current();
    if (t) {
        t->state = THREAD_DEAD;
        if (t->proc) {
            t->proc->state = PROC_ZOMBIE;
            t->proc->exit_code = status;
        }
        serial_puts("exit: pid=");
        if (t->proc) serial_putchar('0' + (t->proc->pid % 10));
        serial_puts(" status=");
        serial_putchar('0' + (status & 0xF));
        serial_putchar('\n');

        extern uint64_t pml4[];
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(t);
    }
    __asm__ volatile("cli; hlt");
}

/* ── SYS_close (3) ───────────────────────────────── */

static long do_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    return fd_close(&p->fds, fd);
}

/* ── SYS_uname (63) ─────────────────────────────── */

struct utsname {
    char sysname[65]; char nodename[65]; char release[65];
    char version[65]; char machine[65]; char domainname[65];
};

static void kstrcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static long do_uname(struct utsname *buf) {
    kstrcpy(buf->sysname, "CosmoRT", 65);
    kstrcpy(buf->nodename, "cosmo", 65);
    kstrcpy(buf->release, "0.1.0", 65);
    kstrcpy(buf->version, "CosmoRT 0.1", 65);
    kstrcpy(buf->machine, "x86_64", 65);
    kstrcpy(buf->domainname, "", 65);
    return 0;
}

/* ── SYS_getrandom (318) ────────────────────────── */

static long do_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < buflen; i++) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        p[i] = (uint8_t)(lo ^ (lo >> 8) ^ hi ^ (uint8_t)i);
    }
    return (long)buflen;
}

/* ── SYS_clock_gettime (228) / SYS_clock_getres (229) ── */

struct k_timespec { long tv_sec; long tv_nsec; };
struct k_timeval  { long tv_sec; long tv_usec; };

static long do_clock_gettime(int clk_id, struct k_timespec *tp) {
    (void)clk_id; /* CLOCK_REALTIME and CLOCK_MONOTONIC both use uptime */
    if (!tp) return -EFAULT;
    uint64_t ms = timer_ms();
    tp->tv_sec = (long)(ms / 1000);
    tp->tv_nsec = (long)((ms % 1000) * 1000000);
    return 0;
}

static long do_clock_getres(int clk_id, struct k_timespec *tp) {
    (void)clk_id;
    if (tp) { tp->tv_sec = 0; tp->tv_nsec = 1000000; } /* 1ms resolution */
    return 0;
}

static long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    if (!req) return -EFAULT;
    uint64_t ms = (uint64_t)req->tv_sec * 1000 + (uint64_t)(req->tv_nsec / 1000000);
    if (ms == 0) ms = 1;
    timer_sleep_ms((uint32_t)ms);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

static long do_clock_nanosleep(int clk_id, int flags,
                               const struct k_timespec *req, struct k_timespec *rem) {
    (void)clk_id;
    if (flags & TIMER_ABSTIME) {
        if (!req) return -EFAULT;
        uint64_t target_ms = (uint64_t)req->tv_sec * 1000
                           + (uint64_t)(req->tv_nsec / 1000000);
        uint64_t now = timer_ms();
        if (target_ms > now) timer_sleep_ms((uint32_t)(target_ms - now));
        return 0;
    }
    return do_nanosleep(req, rem);
}

static long do_gettimeofday(struct k_timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t ms = timer_ms();
        tv->tv_sec = (long)(ms / 1000);
        tv->tv_usec = (long)((ms % 1000) * 1000);
    }
    return 0;
}

/* ── SYS_sched_setaffinity (203) / getaffinity (204) ── */

static long do_sched_setaffinity(int pid, size_t cpusetsize, const uint64_t *mask) {
    (void)pid; /* pid=0 → current thread */
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    uint64_t m = *mask;
    if (m == 0) return -EINVAL;
    int core = __builtin_ctzll(m);
    if (core >= SMP_MAX_CORES) return -EINVAL;
    t->cpu_affinity = core;
    return 0;
}

static long do_sched_getaffinity(int pid, size_t cpusetsize, uint64_t *mask) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    if (t->cpu_affinity >= 0)
        *mask = 1ULL << t->cpu_affinity;
    else
        *mask = ~0ULL;  /* all 64 cores */
    return (long)sizeof(uint64_t);
}

static long do_sched_yield(void) {
    thread_t *t = thread_current();
    if (t) t->state = THREAD_RUNNABLE;
    return 0;
}

/* ── SYS_sched_setscheduler (144) / getscheduler (145) ── */

struct sched_param_k { int sched_priority; };

static long do_sched_setscheduler(int pid, int policy, const struct sched_param_k *param) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (policy < 0 || policy > 2) return -EINVAL;
    t->sched_policy = policy;
    if (param) t->priority = param->sched_priority;
    return 0;
}

static long do_sched_getscheduler(int pid) {
    (void)pid;
    thread_t *t = thread_current();
    return t ? t->sched_policy : 0;
}

static long do_sched_setparam(int pid, const struct sched_param_k *param) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    t->priority = param->sched_priority;
    return 0;
}

static long do_sched_getparam(int pid, struct sched_param_k *param) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    param->sched_priority = t->priority;
    return 0;
}

/* ── Dispatcher ──────────────────────────────────── */

long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    switch (num) {
    /* I/O */
    case SYS_READ:          return do_read((int)a1, (void *)a2, (size_t)a3);
    case SYS_WRITE:         return do_write((int)a1, (const void *)a2, (size_t)a3);
    case SYS_WRITEV:        return do_writev((int)a1, (const struct iovec *)a2, (int)a3);
    case SYS_CLOSE:         return do_close((int)a1);

    /* Memory */
    case SYS_BRK:           return do_brk((unsigned long)a1);
    case SYS_MMAP:          return do_mmap((unsigned long)a1, (size_t)a2, (int)a3,
                                           (int)a4, (int)a5, a6);
    case SYS_MUNMAP:        return do_munmap((unsigned long)a1, (size_t)a2);
    case SYS_MPROTECT:      return do_mprotect((unsigned long)a1, (size_t)a2, (int)a3);
    case SYS_MLOCK:         return do_mlock((unsigned long)a1, (size_t)a2);
    case SYS_MUNLOCK:       return do_munlock((unsigned long)a1, (size_t)a2);
    case SYS_MLOCKALL:      return do_mlockall((int)a1);
    case SYS_MUNLOCKALL:    return do_munlockall();

    /* Process lifecycle */
    case SYS_EXIT:          do_exit((int)a1); return 0;
    case SYS_EXIT_GROUP:    do_exit((int)a1); return 0;

    /* Thread/TLS */
    case SYS_ARCH_PRCTL:    return do_arch_prctl((int)a1, (unsigned long)a2);
    case SYS_SET_TID_ADDRESS: {
        thread_t *t = thread_current();
        return t ? (long)t->tid : 1;
    }
    case SYS_SET_ROBUST_LIST: return 0;

    /* Signals (stubs) */
    case SYS_RT_SIGACTION:    return 0;
    case SYS_RT_SIGPROCMASK:  return 0;

    /* Identity */
    case SYS_GETPID:  { process_t *p = proc_current(); return p ? (long)p->pid : 1; }
    case SYS_GETTID:  { thread_t *t = thread_current(); return t ? (long)t->tid : 1; }
    case SYS_GETUID:  return 0;
    case SYS_GETGID:  return 0;
    case SYS_GETEUID: return 0;
    case SYS_GETEGID: return 0;

    /* System info */
    case SYS_UNAME:     return do_uname((struct utsname *)a1);
    case SYS_GETRANDOM: return do_getrandom((void *)a1, (size_t)a2, (unsigned int)a3);
    case SYS_PRLIMIT64: return -ENOSYS;
    case SYS_RSEQ:      return -ENOSYS;

    /* Timers / clocks */
    case SYS_CLOCK_GETTIME:   return do_clock_gettime((int)a1, (struct k_timespec *)a2);
    case SYS_CLOCK_GETRES:    return do_clock_getres((int)a1, (struct k_timespec *)a2);
    case SYS_CLOCK_NANOSLEEP: return do_clock_nanosleep((int)a1, (int)a2,
                                       (const struct k_timespec *)a3, (struct k_timespec *)a4);
    case SYS_NANOSLEEP:       return do_nanosleep((const struct k_timespec *)a1,
                                       (struct k_timespec *)a2);
    case SYS_GETTIMEOFDAY:    return do_gettimeofday((struct k_timeval *)a1, (void *)a2);

    /* Scheduling */
    case SYS_SCHED_SETAFFINITY: return do_sched_setaffinity((int)a1, (size_t)a2, (const uint64_t *)a3);
    case SYS_SCHED_GETAFFINITY: return do_sched_getaffinity((int)a1, (size_t)a2, (uint64_t *)a3);
    case SYS_SCHED_YIELD:       return do_sched_yield();
    case SYS_SCHED_SETSCHEDULER: return do_sched_setscheduler((int)a1, (int)a2,
                                          (const struct sched_param_k *)a3);
    case SYS_SCHED_GETSCHEDULER: return do_sched_getscheduler((int)a1);
    case SYS_SCHED_SETPARAM:    return do_sched_setparam((int)a1, (const struct sched_param_k *)a2);
    case SYS_SCHED_GETPARAM:    return do_sched_getparam((int)a1, (struct sched_param_k *)a2);

    /* Futex */
    case SYS_FUTEX:     return do_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                                        (const struct timespec *)a4,
                                        (uint32_t *)a5, (uint32_t)a6);

    /* Stubs */
    case SYS_OPEN:   return -ENOSYS;
    case SYS_FSTAT:  return -ENOSYS;
    case SYS_STAT:   return -ENOSYS;
    case SYS_ACCESS: return -ENOSYS;
    case SYS_IOCTL:  return -ENOSYS;
    case SYS_FCNTL:  return -ENOSYS;

    default:
        serial_puts("syscall: unhandled #");
        serial_hex64((uint64_t)num);
        serial_putchar('\n');
        return -ENOSYS;
    }
}
