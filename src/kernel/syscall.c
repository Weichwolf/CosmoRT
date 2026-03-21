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
#include "slab.h"
#include "vfs.h"
#include "memops.h"
#include "socket.h"
#include "hw.h"
#include "net_port.h"
#include "irq.h"
#include "spinlock.h"
#include "procfs.h"
#include "epoll.h"
#include "pty.h"
#include "vt.h"

/* Validate user pointer: must be in lower half, no overflow */
static inline int user_ok(uint64_t addr, size_t len) {
    return addr < 0x800000000000ULL &&
           addr + len <= 0x800000000000ULL &&
           addr + len >= addr;
}

/* Copy user path string to kernel buffer with full bounds checking.
 * Returns string length (excluding NUL) or negative errno. */
#define PATH_MAX 4096
static int copy_path_from_user(char *kbuf, const char *upath, size_t max) {
    if (!user_ok((uint64_t)upath, 1)) return -EFAULT;
    for (size_t i = 0; i < max; i++) {
        if ((uint64_t)(upath + i) >= 0x800000000000ULL) return -EFAULT;
        kbuf[i] = upath[i];
        if (kbuf[i] == '\0') return (int)i;
    }
    return -ENAMETOOLONG;
}

/* Forward declarations */
static void do_exit(int status);
void check_pending_signals(void);
void check_signals_syscall_path(long *result_ptr, long num);

/* Forward declarations for pipe (implemented before dispatcher) */
struct pipe;
static struct pipe *pipe_from_fd(fd_entry_t *fde, int *is_write);
static long pipe_read(struct pipe *pp, void *buf, size_t count);
static long pipe_write(struct pipe *pp, const void *buf, size_t count);
static long pipe_close(fd_entry_t *fde);

/* Syscall saved frame layout (matches syscall_entry.asm push order) */
typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t rax;       /* syscall number */
    uint64_t r11;       /* user RFLAGS */
    uint64_t rcx;       /* user RIP */
} syscall_frame_t;

/* clone flags */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* PTE flags */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_NX      (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* ── SYS_write (1) ───────────────────────────────── */

static long do_write(int fd, const void *buf, size_t count) {
    if (!user_ok((uint64_t)buf, count)) return -EFAULT;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_SERIAL) {
        size_t actual = count > 0x10000 ? 0x10000 : count;
        uint8_t kbuf[256];
        size_t pos = 0;
        while (pos < actual) {
            size_t chunk = actual - pos > 256 ? 256 : actual - pos;
            kmemcpy(kbuf, (const uint8_t *)buf + pos, chunk);
            for (size_t j = 0; j < chunk; j++) serial_putchar((char)kbuf[j]);
            pos += chunk;
        }
        return (long)actual;
    }
    if (fde->type == FD_FILE)
        return vfs_write(fd, buf, count);
    if (fde->type == FD_SOCKET)
        return socket_write(fd, buf, (long)count);
    if (fde->type == FD_PIPE) {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp || !is_write) return -EBADF;
        return pipe_write(pp, buf, count);
    }
    if (fde->type == FD_EVENTFD)
        return eventfd_write(fde->obj, buf, (long)count);
    if (fde->type == FD_PTY_SLAVE) {
        int pty_id = (int)(long)fde->obj;
        uint8_t kbuf[256];
        size_t pos = 0;
        size_t actual = count > 0x10000 ? 0x10000 : count;
        while (pos < actual) {
            size_t chunk = actual - pos > 256 ? 256 : actual - pos;
            kmemcpy(kbuf, (const uint8_t *)buf + pos, chunk);
            int w = pty_slave_write(pty_id, (const char *)kbuf, (int)chunk);
            if (w <= 0) break;
            pos += (size_t)w;
        }
        /* Flush VT output for immediate rendering */
        vt_flush(pty_id);
        return (long)pos;
    }
    return -EBADF;
}

/* ── SYS_writev (20) ────────────────────────────── */

struct iovec { const void *iov_base; size_t iov_len; };

static long do_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 16) return -EINVAL;
    if (!user_ok((uint64_t)iov, (size_t)iovcnt * sizeof(struct iovec))) return -EFAULT;
    /* Copy iov array to kernel stack to prevent TOCTOU */
    struct iovec k_iov[16];
    kmemcpy(k_iov, iov, (size_t)iovcnt * sizeof(struct iovec));
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!user_ok((uint64_t)k_iov[i].iov_base, k_iov[i].iov_len)) return -EFAULT;
        long r = do_write(fd, (void *)k_iov[i].iov_base, k_iov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((size_t)r < k_iov[i].iov_len) break;
    }
    return total;
}

/* ── SYS_read (0) ────────────────────────────────── */

static long do_read(int fd, void *buf, size_t count) {
    if (!user_ok((uint64_t)buf, count)) return -EFAULT;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_SERIAL) {
        extern char serial_getchar(void);
        uint8_t kbuf[256];
        size_t got = 0;
        while (got < count && got < 256) {
            char c = serial_getchar();
            if (c == 0) break;
            kbuf[got++] = (uint8_t)c;
        }
        kmemcpy(buf, kbuf, got);
        return (long)got;
    }
    if (fde->type == FD_FILE)
        return vfs_read(fd, buf, count);
    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (!pf) return -EBADF;
        /* Read into kernel buffer, then copy to user */
        char kbuf[4096];
        int want = (int)count;
        if (want > (int)sizeof(kbuf)) want = (int)sizeof(kbuf);
        int got = procfs_read(pf->handle, kbuf, want, pf->offset);
        if (got > 0) {
            kmemcpy(buf, kbuf, (size_t)got);
            pf->offset += got;
        }
        return (long)got;
    }
    if (fde->type == FD_SOCKET)
        return socket_read(fd, buf, (long)count);
    if (fde->type == FD_PIPE) {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp || is_write) return -EBADF;
        return pipe_read(pp, buf, count);
    }
    if (fde->type == FD_EVENTFD)
        return eventfd_read(fde->obj, buf, (long)count);
    if (fde->type == FD_TIMERFD)
        return timerfd_read(fde->obj, buf, (long)count);
    if (fde->type == FD_PTY_SLAVE) {
        int pty_id = (int)(long)fde->obj;
        uint8_t kbuf[256];
        size_t want = count > 256 ? 256 : count;
        int got = pty_slave_read(pty_id, (char *)kbuf, (int)want);
        if (got > 0) kmemcpy(buf, kbuf, (size_t)got);
        /* Flush VT after read — renders echo from line discipline */
        extern void vt_flush(int vt_id);
        vt_flush(pty_id);
        return got > 0 ? (long)got : (long)-EAGAIN;
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
    for (uint64_t va = start; va < end; va += 4096) {
        /* Check if already mapped */
        int pml4i = (va >> 39) & 0x1FF;
        int mapped = 0;
        if (user_pml4[pml4i] & PTE_PRESENT) {
            uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PTE_ADDR_MASK);
            int pdpti = (va >> 30) & 0x1FF;
            if (pdpt[pdpti] & PTE_PRESENT) {
                uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);
                int pdi = (va >> 21) & 0x1FF;
                if (pd[pdi] & PTE_PRESENT) {
                    uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PTE_ADDR_MASK);
                    int pti = (va >> 12) & 0x1FF;
                    if (pt[pti] & PTE_PRESENT) mapped = 1;
                }
            }
        }
        if (!mapped) {
            uint64_t *page = alloc_page();
            if (page)
                map_user_page(user_pml4, va, virt_to_phys(page), prot);
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
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    /* Validate file-backed mmap parameters */
    int is_file = (fd >= 0 && !(flags & MAP_ANONYMOUS));
    struct vfs_file *vf = 0;
    if (is_file) {
        fd_entry_t *fde = fd_get(&p->fds, fd);
        if (!fde || fde->type != FD_FILE) return -EBADF;
        vf = (struct vfs_file *)fde->obj;
        if (!vf) return -EBADF;
        if (offset < 0) return -EINVAL;
    }

    length = (length + 0xFFF) & ~0xFFFULL;
    if (length == 0) return -EINVAL;
    if (addr + length < addr) return -EINVAL; /* overflow */

    uint64_t vaddr;
    if (addr && (flags & MAP_FIXED)) {
        vaddr = addr & ~0xFFFULL;
        /* Remove any overlapping VMAs in [vaddr, vaddr+length) */
        for (;;) {
            vma_t *ov = vma_find(p->vma_root, vaddr);
            if (!ov) {
                int found = 0;
                for (uint64_t probe = vaddr; probe < vaddr + length; probe += 4096) {
                    ov = vma_find(p->vma_root, probe);
                    if (ov) { found = 1; break; }
                }
                if (!found) break;
            }
            if (ov->start >= vaddr + length) break;
            if (ov->start < vaddr && ov->end > vaddr + length) {
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
            ov = vma_find(p->vma_root, vaddr);
            if (!ov) break;
            if (ov->start >= vaddr + length) break;
        }
    } else {
        vaddr = vma_find_free(p->vma_root, p->mmap_next, length);
        if (!vaddr) return -ENOMEM;
        p->mmap_next = vaddr;
    }

    /* Create VMA */
    int vma_flags = flags;
    if (p->mlockall_flags & MCL_FUTURE) vma_flags |= VMA_LOCKED;
    vma_t *v = vma_insert(&p->vma_root, vaddr, vaddr + length, prot, vma_flags);
    if (!v) return -ENOMEM;

    /* File-backed mmap: allocate pages and read file content */
    if (is_file) {
        extern long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t off);
        uint64_t file_off = (uint64_t)offset;
        for (uint64_t va = vaddr; va < vaddr + length; va += 4096) {
            uint64_t *pg = alloc_page(); /* zeroed */
            if (!pg) return -ENOMEM;
            /* Read up to 4096 bytes from file at current offset */
            long nread = vfs_pread(vf, pg, 4096, file_off);
            (void)nread; /* short read is fine — rest is zero */
            if (map_user_page(p->pml4, va, virt_to_phys(pg), prot) < 0) {
                page_free(pg);
                return -ENOMEM;
            }
            file_off += 4096;
        }
        return (long)vaddr;
    }

    /* Anonymous: pre-fault if locked */
    if (vma_flags & VMA_LOCKED)
        prefault_range(p->pml4, vaddr, vaddr + length, prot);

    return (long)vaddr;
}

/* ── Page table walk helpers for unmap/mprotect ──── */

static void unmap_range(uint64_t *user_pml4, uint64_t start, uint64_t end) {
    /* PTE physical address mask: bits 12..51 (strips NX, available bits) */
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);

        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & 0x000FFFFFFFFFF000ULL; /* bits 12..51 */
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
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    uint64_t new_flags = prot_to_pte_flags(prot);
    for (uint64_t va = start; va < end; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(user_pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(user_pml4[pml4i] & PHYS_MASK);

        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);

        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);

        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & PHYS_MASK;
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
    if (addr + length < addr) return -EINVAL; /* overflow */
    uint64_t start = addr;
    uint64_t end = addr + length;

    /* Unmap physical pages */
    unmap_range(p->pml4, start, end);
    /* TLB flush: local + cross-core IPI shootdown */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    tlb_shootdown(virt_to_phys(p->pml4));

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
    /* TLB shootdown for other cores sharing this address space */
    tlb_shootdown(virt_to_phys(p->pml4));

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
        thread_t *t = thread_current();
        if (t) t->fs_base = addr;
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
        if (!user_ok(addr, 8)) return -EFAULT;
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        uint64_t val = ((uint64_t)hi << 32) | lo;
        kmemcpy((void *)addr, &val, 8);
        return 0;
    }
    return -EINVAL;
}

/* ── SYS_exit / SYS_exit_group ───────────────────── */

static void do_exit(int status) {
    thread_t *t = thread_current();
    if (t) {
        process_t *p = t->proc;
        t->state = THREAD_DEAD;
        if (p) {
            net_port_check_driver((int)p->pid);
            p->state = PROC_ZOMBIE;
            p->exit_code = status;

            serial_puts("exit: pid=");
            serial_putchar('0' + (p->pid % 10));
            serial_puts(" status=");
            serial_putchar('0' + (status & 0xF));
            serial_putchar('\n');

            /* Wake parent if blocked in wait4 */
            if (p->parent_pid) {
                process_t *parent = proc_find(p->parent_pid);
                if (parent) {
                    /* Find and wake blocked threads in parent */
                    thread_t *pt = parent->threads;
                    while (pt) {
                        if (pt->state == THREAD_BLOCKED) {
                            extern void sched_add(thread_t *t);
                            sched_add(pt);
                        }
                        pt = pt->proc_next;
                    }
                }
            }
        }

        extern uint64_t pml4[];
        __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(pml4)) : "memory");
        thread_return_to_kernel(t);
    }
    __asm__ volatile("cli; hlt");
}

/* ── SYS_clone (56) ──────────────────────────────── */

static long do_clone(unsigned long flags, void *child_stack,
                     int *parent_tid, int *child_tid, unsigned long tls) {
    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || !cur->proc) return -EFAULT;

    /* Read parent's saved user registers from the syscall frame */
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;

    /* Allocate new thread */
    thread_t *t = thread_alloc();
    if (!t) return -ENOMEM;

    /* Copy parent's register state */
    t->rip    = frame->rcx;    /* user RIP (SYSCALL saved in RCX) */
    t->rflags = frame->r11;    /* user RFLAGS (SYSCALL saved in R11) */
    t->rax    = 0;             /* clone() returns 0 in child */
    t->rbx    = frame->rbx;
    t->rcx    = frame->rcx;
    t->rdx    = frame->rdx;
    t->rsi    = frame->rsi;
    t->rdi    = frame->rdi;
    t->rbp    = frame->rbp;
    t->r8     = frame->r8;
    t->r9     = frame->r9;
    t->r10    = frame->r10;
    t->r11    = frame->r11;
    t->r12    = frame->r12;
    t->r13    = frame->r13;
    t->r14    = frame->r14;
    t->r15    = frame->r15;

    /* Child stack */
    t->rsp = child_stack ? (uint64_t)child_stack : frame->rsi; /* RSI had arg2 */

    /* Share address space (CLONE_VM) */
    t->proc = cur->proc;
    t->state = THREAD_RUNNABLE;
    t->sched_policy = cur->sched_policy;
    t->priority = cur->priority;
    t->cpu_affinity = -1;
    t->timeslice = RR_TIMESLICE;

    /* Kernel stack for new thread */
    t->kstack = (uint8_t *)pages_alloc(KSTACK_SIZE / 4096);
    if (!t->kstack) { thread_free(t); return -ENOMEM; }
    t->kstack_top = (uint64_t)(uintptr_t)(t->kstack + KSTACK_SIZE);

    /* TLS (CLONE_SETTLS) */
    if (flags & CLONE_SETTLS) {
        t->fs_base = tls;
    } else {
        t->fs_base = cur->fs_base;
    }

    /* Parent TID (CLONE_PARENT_SETTID) */
    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        if (!user_ok((uint64_t)parent_tid, 4)) { thread_free(t); return -EFAULT; }
        *parent_tid = t->tid;
    }

    /* Child TID (CLONE_CHILD_SETTID) */
    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        if (!user_ok((uint64_t)child_tid, 4)) { thread_free(t); return -EFAULT; }
        *child_tid = t->tid;
    }

    /* Add to process thread list (under lock for concurrent clone safety) */
    {
        uint64_t lflags;
        spin_lock_irq(&cur->proc->lock, &lflags);
        t->proc_next = cur->proc->threads;
        cur->proc->threads = t;
        cur->proc->thread_count++;
        spin_unlock_irq(&cur->proc->lock, lflags);
    }

    /* Add to scheduler */
    extern void sched_add(thread_t *t);
    sched_add(t);

    return (long)t->tid;
}

/* ── SYS_close (3) ───────────────────────────────── */

static long do_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;
    if (fde->type == FD_FILE)
        return vfs_close(fd);
    if (fde->type == FD_PROCFS) {
        procfs_fd_t *pf = (procfs_fd_t *)fde->obj;
        if (pf) {
            procfs_close(pf->handle);
            procfs_fd_free(pf);
        }
        return fd_close(&p->fds, fd);
    }
    if (fde->type == FD_SOCKET)
        return socket_close(fd);
    if (fde->type == FD_PIPE) {
        long r = pipe_close(fde);
        fd_close(&p->fds, fd);
        return r;
    }
    if (fde->type == FD_EPOLL)   { epoll_destroy(fde->obj);   return fd_close(&p->fds, fd); }
    if (fde->type == FD_EVENTFD) { eventfd_destroy(fde->obj); return fd_close(&p->fds, fd); }
    if (fde->type == FD_TIMERFD) { timerfd_destroy(fde->obj); return fd_close(&p->fds, fd); }
    if (fde->type == FD_INOTIFY) { inotify_destroy(fde->obj); return fd_close(&p->fds, fd); }
    if (fde->type == FD_PTY_MASTER || fde->type == FD_PTY_SLAVE)
        return fd_close(&p->fds, fd);
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
    if (!user_ok((uint64_t)buf, sizeof(struct utsname))) return -EFAULT;
    struct utsname kbuf;
    kstrcpy(kbuf.sysname, "CosmoRT", 65);
    kstrcpy(kbuf.nodename, "cosmo", 65);
    kstrcpy(kbuf.release, "0.1.0", 65);
    kstrcpy(kbuf.version, "CosmoRT 0.1", 65);
    kstrcpy(kbuf.machine, "x86_64", 65);
    kstrcpy(kbuf.domainname, "", 65);
    kmemcpy(buf, &kbuf, sizeof(struct utsname));
    return 0;
}

/* ── SYS_getrandom (318) ────────────────────────── */

static long do_getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if (!user_ok((uint64_t)buf, buflen)) return -EFAULT;
    if (buflen > 256) buflen = 256; /* cap kernel stack usage */
    uint8_t kbuf[256];
    extern int random_get(void *buf, size_t len);
    if (random_get(kbuf, buflen) < 0) return -EIO;
    kmemcpy(buf, kbuf, buflen);
    return (long)buflen;
}

/* ── SYS_clock_gettime (228) / SYS_clock_getres (229) ── */

/* struct k_timespec defined in epoll.h (K_TIMESPEC_DEFINED guard) */
struct k_timeval  { long tv_sec; long tv_usec; };

static long do_clock_gettime(int clk_id, struct k_timespec *tp) {
    (void)clk_id; /* CLOCK_REALTIME and CLOCK_MONOTONIC both use uptime */
    if (!tp || !user_ok((uint64_t)tp, 16)) return -EFAULT;
    struct k_timespec kts;
    uint64_t ms = timer_ms();
    kts.tv_sec = (long)(ms / 1000);
    kts.tv_nsec = (long)((ms % 1000) * 1000000);
    kmemcpy(tp, &kts, sizeof(kts));
    return 0;
}

static long do_clock_getres(int clk_id, struct k_timespec *tp) {
    (void)clk_id;
    if (tp && !user_ok((uint64_t)tp, sizeof(struct k_timespec))) return -EFAULT;
    if (tp) {
        struct k_timespec kts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
        kmemcpy(tp, &kts, sizeof(kts));
    }
    return 0;
}

static long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    if (!req || !user_ok((uint64_t)req, 16)) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;
    struct k_timespec kreq;
    kmemcpy(&kreq, req, sizeof(kreq));
    uint64_t ms = (uint64_t)kreq.tv_sec * 1000 + (uint64_t)(kreq.tv_nsec / 1000000);
    if (ms == 0) ms = 1;
    timer_sleep_ms((uint32_t)ms);
    if (rem) {
        struct k_timespec krem = { .tv_sec = 0, .tv_nsec = 0 };
        kmemcpy(rem, &krem, sizeof(krem));
    }
    return 0;
}

static long do_clock_nanosleep(int clk_id, int flags,
                               const struct k_timespec *req, struct k_timespec *rem) {
    (void)clk_id;
    if (req && !user_ok((uint64_t)req, 16)) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;
    if (flags & TIMER_ABSTIME) {
        if (!req) return -EFAULT;
        struct k_timespec kreq;
        kmemcpy(&kreq, req, sizeof(kreq));
        uint64_t target_ms = (uint64_t)kreq.tv_sec * 1000
                           + (uint64_t)(kreq.tv_nsec / 1000000);
        uint64_t now = timer_ms();
        if (target_ms > now) timer_sleep_ms((uint32_t)(target_ms - now));
        return 0;
    }
    return do_nanosleep(req, rem);
}

static long do_gettimeofday(struct k_timeval *tv, void *tz) {
    (void)tz;
    if (tv && !user_ok((uint64_t)tv, 16)) return -EFAULT;
    if (tv) {
        struct k_timeval ktv;
        uint64_t ms = timer_ms();
        ktv.tv_sec = (long)(ms / 1000);
        ktv.tv_usec = (long)((ms % 1000) * 1000);
        kmemcpy(tv, &ktv, sizeof(ktv));
    }
    return 0;
}

/* ── SYS_sched_setaffinity (203) / getaffinity (204) ── */

static long do_sched_setaffinity(int pid, size_t cpusetsize, const uint64_t *mask) {
    (void)pid; /* pid=0 → current thread */
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    if (!user_ok((uint64_t)mask, cpusetsize)) return -EFAULT;
    uint64_t k_mask;
    kmemcpy(&k_mask, mask, 8);
    if (k_mask == 0) return -EINVAL;
    int core = __builtin_ctzll(k_mask);
    if (core >= SMP_MAX_CORES) return -EINVAL;
    t->cpu_affinity = core;
    return 0;
}

static long do_sched_getaffinity(int pid, size_t cpusetsize, uint64_t *mask) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    if (!user_ok((uint64_t)mask, cpusetsize)) return -EFAULT;
    uint64_t kmask;
    if (t->cpu_affinity >= 0)
        kmask = 1ULL << t->cpu_affinity;
    else
        kmask = ~0ULL;  /* all 64 cores */
    kmemcpy(mask, &kmask, 8);
    return (long)sizeof(uint64_t);
}

/* Save user register state from syscall frame into thread_t.
 * Used by clone, futex_wait, and any syscall that blocks. */
void save_user_state_for_block(thread_t *t, long return_value) {
    percpu_t *cpu = percpu_self();
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    t->rip    = frame->rcx;       /* user RIP */
    t->rflags = frame->r11;       /* user RFLAGS */
    t->rsp    = cpu->user_rsp;    /* user RSP */
    t->rax    = (uint64_t)return_value;
    t->rbx = frame->rbx; t->rcx = frame->rcx; t->rdx = frame->rdx;
    t->rsi = frame->rsi; t->rdi = frame->rdi; t->rbp = frame->rbp;
    t->r8  = frame->r8;  t->r9  = frame->r9;  t->r10 = frame->r10;
    t->r11 = frame->r11; t->r12 = frame->r12; t->r13 = frame->r13;
    t->r14 = frame->r14; t->r15 = frame->r15;
}

static long do_sched_yield(void) {
    return 0;  /* hint only — timer preemption handles actual switching */
}

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

/* ── Signal frame layout (Linux-compatible) ──────────────── */

/* mcontext_t register layout (Linux x86_64 compatible) */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, rflags;
    uint64_t cs, gs, fs, err, trapno, oldmask, cr2; /* zeroed */
} sig_mcontext_t; /* 25 * 8 = 200 bytes */

typedef struct {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp, ss_flags, ss_size; /* uc_stack */
    sig_mcontext_t uc_mcontext;
    uint64_t uc_sigmask;
} sig_ucontext_t; /* 8 + 8 + 24 + 200 + 8 = 248 bytes */

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

/* Total signal frame: restorer_addr(8) + siginfo(128) + ucontext(248) + trampoline(9) */
#define SIG_FRAME_SIZE  (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t) + sizeof(sig_trampoline))

/* Offsets from new RSP (bottom of frame):
 *   [RSP+0]   = return address (restorer or trampoline)
 *   [RSP+8]   = siginfo_t
 *   [RSP+136] = ucontext_t
 *   [RSP+384] = trampoline bytes (if no sa_restorer)
 */
#define SIGFRAME_OFF_RETADDR   0
#define SIGFRAME_OFF_SIGINFO   8
#define SIGFRAME_OFF_UCONTEXT  (8 + sizeof(sig_siginfo_t))
#define SIGFRAME_OFF_TRAMPOLINE (8 + sizeof(sig_siginfo_t) + sizeof(sig_ucontext_t))

/* Deliver signal to user thread by pushing signal frame onto user stack.
 * Modifies thread registers so next return-to-userspace enters the handler.
 * Called from check_pending_signals (SYSCALL return or timer preempt path). */
static void deliver_signal(thread_t *t, int signo) {
    process_t *p = t->proc;
    struct k_sigaction *sa = &p->sig_actions[signo];

    if ((uint64_t)sa->sa_handler <= 1) return; /* SIG_DFL or SIG_IGN — shouldn't be here */

    /* Compute frame location on user stack */
    uint64_t frame_size = SIG_FRAME_SIZE;
    /* Add trampoline only if no sa_restorer */
    int has_restorer = (sa->sa_flags & SA_RESTORER) && sa->sa_restorer;
    if (has_restorer)
        frame_size -= sizeof(sig_trampoline);
    uint64_t new_rsp = (t->rsp - frame_size) & ~0xFULL; /* 16-byte align */

    /* Verify target stack area is in a writable VMA */
    vma_t *vma = vma_find(p->vma_root, new_rsp);
    if (!vma || new_rsp < vma->start || (new_rsp + frame_size) > vma->end
        || !(vma->prot & PROT_WRITE)) {
        do_exit(128 + signo);
        return;
    }

    /* Ensure all pages in the frame are mapped */
    for (uint64_t addr = new_rsp & ~0xFFFULL; addr < new_rsp + frame_size; addr += 4096) {
        if (ensure_user_page(p, addr) < 0) {
            /* Can't allocate stack page — kill process */
            do_exit(128 + signo);
            return;
        }
    }

    /* Build ucontext (save current registers) */
    sig_ucontext_t uc;
    kmemset(&uc, 0, sizeof(uc));
    uc.uc_mcontext.r8  = t->r8;  uc.uc_mcontext.r9  = t->r9;
    uc.uc_mcontext.r10 = t->r10; uc.uc_mcontext.r11 = t->r11;
    uc.uc_mcontext.r12 = t->r12; uc.uc_mcontext.r13 = t->r13;
    uc.uc_mcontext.r14 = t->r14; uc.uc_mcontext.r15 = t->r15;
    uc.uc_mcontext.rdi = t->rdi; uc.uc_mcontext.rsi = t->rsi;
    uc.uc_mcontext.rbp = t->rbp; uc.uc_mcontext.rbx = t->rbx;
    uc.uc_mcontext.rdx = t->rdx; uc.uc_mcontext.rax = t->rax;
    uc.uc_mcontext.rcx = t->rcx; uc.uc_mcontext.rsp = t->rsp;
    uc.uc_mcontext.rip = t->rip; uc.uc_mcontext.rflags = t->rflags;
    uc.uc_sigmask = p->sig_blocked;

    /* Build siginfo */
    sig_siginfo_t si;
    kmemset(&si, 0, sizeof(si));
    si.si_signo = (int32_t)signo;
    si.si_code = 0; /* SI_USER */

    /* Write return address */
    uint64_t restorer_addr;
    if (has_restorer) {
        restorer_addr = (uint64_t)sa->sa_restorer;
    } else {
        restorer_addr = new_rsp + SIGFRAME_OFF_TRAMPOLINE;
    }

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
    t->rsi = new_rsp + SIGFRAME_OFF_SIGINFO;
    t->rdx = new_rsp + SIGFRAME_OFF_UCONTEXT;
    /* Clear direction flag, keep interrupts enabled */
    t->rflags &= ~(1ULL << 10); /* DF=0 */
    t->rflags |= (1ULL << 9);   /* IF=1 */

    /* Block this signal during handler + sa_mask */
    p->sig_blocked |= (1ULL << signo) | sa->sa_mask;
    /* SIGKILL/SIGSTOP never blocked */
    p->sig_blocked &= ~((1ULL << 9) | (1ULL << 19));
}

/* Check and deliver pending signals. Operates on thread_t register fields.
 * Callers must sync hardware frame ↔ thread_t before/after. */
void check_pending_signals(void) {
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;

    uint64_t deliverable = p->sig_pending & ~p->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < 32; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;
        p->sig_pending &= ~(1ULL << sig);

        struct k_sigaction *sa = &p->sig_actions[sig];
        uint64_t handler = (uint64_t)sa->sa_handler;

        if (handler == 1) continue; /* SIG_IGN */

        if (handler == 0) {
            /* SIG_DFL */
            /* SIGCHLD (17): default is ignore */
            if (sig == 17) continue;
            /* Fatal signals: SIGKILL=9, SIGSEGV=11, SIGPIPE=13, SIGTERM=15, SIGABRT=6 */
            if (sig == 9 || sig == 11 || sig == 13 || sig == 15 || sig == 6) {
                do_exit(128 + sig); /* doesn't return */
            }
            /* Others: default ignore */
            continue;
        }

        /* User handler — deliver via thread_t modification */
        deliver_signal(t, sig);
        return; /* deliver one signal at a time */
    }
}

/* ── SYS_open (2) / SYS_openat (257) ────────────────── */

static long do_open(const char *path, int flags, int mode) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_open(kpath, flags, mode);
}

static long do_openat(int dirfd, const char *path, int flags, int mode) {
    /* Only AT_FDCWD (-100) supported for now */
    (void)dirfd;
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_open(kpath, flags, mode);
}

/* ── SYS_lseek (8) ──────────────────────────────── */

static long do_lseek(int fd, long offset, int whence) {
    return vfs_lseek(fd, offset, whence);
}

/* ── SYS_fstat (5) / SYS_stat (4) ───────────────── */

static long do_fstat(int fd, struct k_stat *buf) {
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    return vfs_fstat(fd, buf);
}

static long do_stat(const char *path, struct k_stat *buf) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    return vfs_stat(kpath, buf);
}

/* ── SYS_dup2 (33) / SYS_dup3 (292) ────────────── */

static long do_dup2(int oldfd, int newfd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -EBADF;
    fd_entry_t *old = fd_get(&p->fds, oldfd);
    if (!old) return -EBADF;
    if (oldfd == newfd) return newfd;

    /* Close newfd if open */
    fd_entry_t *cur = fd_get(&p->fds, newfd);
    if (cur) {
        if (cur->type == FD_FILE) vfs_close(newfd);
        else fd_close(&p->fds, newfd);
    }

    /* Copy the fd entry */
    p->fds.entries[newfd] = *old;
    if (newfd >= p->fds.max_fd) p->fds.max_fd = newfd + 1;
    return newfd;
}

static long do_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -EBADF;
    fd_entry_t *old = fd_get(&p->fds, oldfd);
    if (!old) return -EBADF;

    /* Close newfd if open */
    fd_entry_t *cur = fd_get(&p->fds, newfd);
    if (cur) {
        if (cur->type == FD_FILE) vfs_close(newfd);
        else fd_close(&p->fds, newfd);
    }

    /* Copy the fd entry */
    p->fds.entries[newfd] = *old;
    if (flags & O_CLOEXEC)
        p->fds.entries[newfd].flags |= O_CLOEXEC;
    if (newfd >= p->fds.max_fd) p->fds.max_fd = newfd + 1;
    return newfd;
}

/* ── SYS_getcwd (79) / SYS_chdir (80) ──────────── */

static long do_getcwd(char *buf, size_t size) {
    if (!user_ok((uint64_t)buf, size)) return -EFAULT;
    int r = vfs_getcwd(buf, size);
    if (r < 0) return r;
    return (long)(uint64_t)buf; /* Linux returns pointer */
}

static long do_chdir(const char *path) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_chdir(kpath);
}

/* ── Signals (2.4) ───────────────────────────────── */

#define SIG_DFL  ((void *)0)
#define SIG_IGN  ((void *)1)

/* SIGKILL=9, SIGSEGV=11, SIGPIPE=13, SIGCHLD=17, SIGTERM=15 */

static long do_rt_sigaction(int sig, const struct k_sigaction *act,
                            struct k_sigaction *oldact, size_t sigsetsize) {
    (void)sigsetsize;
    if (sig < 1 || sig >= 32) return -EINVAL;
    if (sig == 9) return -EINVAL; /* SIGKILL cannot be caught */

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    if (oldact) {
        if (!user_ok((uint64_t)oldact, sizeof(struct k_sigaction))) return -EFAULT;
        *oldact = p->sig_actions[sig];
    }

    if (act) {
        if (!user_ok((uint64_t)act, sizeof(struct k_sigaction))) return -EFAULT;
        struct k_sigaction k_act;
        kmemcpy(&k_act, act, sizeof(k_act));
        p->sig_actions[sig] = k_act;
    }

    return 0;
}

static long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset,
                              size_t sigsetsize) {
    (void)sigsetsize;
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    if (oldset) {
        if (!user_ok((uint64_t)oldset, 8)) return -EFAULT;
        *oldset = p->sig_blocked;
    }

    if (set) {
        if (!user_ok((uint64_t)set, 8)) return -EFAULT;
        uint64_t k_set;
        kmemcpy(&k_set, set, 8);
        uint64_t mask = k_set;
        mask &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL, SIGSTOP cannot be blocked */
        switch (how) {
        case 0: p->sig_blocked |= mask; break;  /* SIG_BLOCK */
        case 1: p->sig_blocked &= ~mask; break; /* SIG_UNBLOCK */
        case 2: p->sig_blocked = mask; break;    /* SIG_SETMASK */
        default: return -EINVAL;
        }
    }

    return 0;
}

static long do_kill(int pid, int sig) {
    if (sig < 0 || sig >= 32) return -EINVAL;
    if (sig == 0) return 0; /* check permission only */

    process_t *target = 0;
    if (pid > 0) {
        target = proc_find((uint32_t)pid);
    } else if (pid == 0 || pid == -1) {
        /* Signal to self or all — just handle self */
        target = proc_current();
    }
    if (!target) return -ESRCH;

    /* Check handler */
    void *handler = target->sig_actions[sig].sa_handler;
    if (handler == SIG_IGN) return 0;

    /* SIG_DFL: kill the process for fatal signals */
    if (handler == SIG_DFL) {
        /* SIGCHLD default = ignore */
        if (sig == 17) return 0; /* SIGCHLD */

        /* Fatal signals: kill the process */
        target->state = PROC_ZOMBIE;
        target->exit_code = sig;
        target->sig_pending |= (1ULL << sig);

        /* If target has blocked threads, wake them to die */
        thread_t *t = target->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED || t->state == THREAD_RUNNING) {
                t->state = THREAD_DEAD;
            }
            t = t->proc_next;
        }
        return 0;
    }

    /* User handler registered — set pending bit.
     * Delivery happens on return to userspace via check_pending_signals. */
    target->sig_pending |= (1ULL << sig);
    return 0;
}

/* ── SYS_RT_SIGRETURN (15) ──────────────────────────── */

static long do_rt_sigreturn(void) {
    percpu_t *cpu = percpu_self();
    thread_t *t = cpu->current_thread;
    if (!t || !t->proc) return -EFAULT;
    process_t *p = t->proc;

    /* After the handler did `ret`, RSP points past the return address.
     * The restorer then called `syscall` for SYS_RT_SIGRETURN.
     * The user RSP at syscall entry = restorer's RSP.
     * But the restorer is a simple `mov rax,15; syscall` — no stack ops.
     * So user RSP = frame base + 8 (return addr was popped by handler's `ret`).
     * We need to find the signal frame at user_rsp - 8. */
    uint64_t frame_rsp = cpu->user_rsp - 8;

    /* Read ucontext from the signal frame (direct access — no SMAP) */
    uint64_t uc_addr = frame_rsp + SIGFRAME_OFF_UCONTEXT;
    if (!user_ok(uc_addr, sizeof(sig_ucontext_t))) return -EFAULT;
    sig_ucontext_t uc;
    kmemcpy(&uc, (const void *)uc_addr, sizeof(uc));

    /* Restore registers from ucontext into syscall frame.
     * The SYSRET epilog in syscall_entry.asm will pop these. */
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    frame->r15 = uc.uc_mcontext.r15; frame->r14 = uc.uc_mcontext.r14;
    frame->r13 = uc.uc_mcontext.r13; frame->r12 = uc.uc_mcontext.r12;
    frame->rbp = uc.uc_mcontext.rbp; frame->rbx = uc.uc_mcontext.rbx;
    frame->r9  = uc.uc_mcontext.r9;  frame->r8  = uc.uc_mcontext.r8;
    frame->r10 = uc.uc_mcontext.r10; frame->rdx = uc.uc_mcontext.rdx;
    frame->rsi = uc.uc_mcontext.rsi; frame->rdi = uc.uc_mcontext.rdi;
    frame->rax = uc.uc_mcontext.rax;
    frame->r11 = uc.uc_mcontext.rflags; /* SYSRET restores RFLAGS from R11 */
    frame->rcx = uc.uc_mcontext.rip;    /* SYSRET restores RIP from RCX */
    cpu->user_rsp = uc.uc_mcontext.rsp;

    /* Restore signal mask */
    p->sig_blocked = uc.uc_sigmask;
    p->sig_blocked &= ~((1ULL << 9) | (1ULL << 19)); /* SIGKILL/SIGSTOP never blocked */

    /* Return value doesn't matter — RAX is restored from ucontext.
     * But the syscall_entry.asm overwrites RAX with our return value AFTER
     * we set frame->rax. We need RAX to be the saved value.
     * Trick: return the saved RAX so it ends up correct. */
    return (long)uc.uc_mcontext.rax;
}

/* ── SYS_sched_setscheduler (144) / getscheduler (145) ── */

struct sched_param_k { int sched_priority; };

static long do_sched_setscheduler(int pid, int policy, const struct sched_param_k *param) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (policy < 0 || policy > 2) return -EINVAL;
    t->sched_policy = policy;
    if (param) {
        if (!user_ok((uint64_t)param, sizeof(struct sched_param_k))) return -EFAULT;
        struct sched_param_k kp;
        kmemcpy(&kp, param, sizeof(kp));
        if (kp.sched_priority < 0 || kp.sched_priority >= PRIO_LEVELS) return -EINVAL;
        t->priority = kp.sched_priority;
    }
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
    if (!user_ok((uint64_t)param, sizeof(struct sched_param_k))) return -EFAULT;
    struct sched_param_k kp;
    kmemcpy(&kp, param, sizeof(kp));
    if (kp.sched_priority < 0 || kp.sched_priority >= PRIO_LEVELS) return -EINVAL;
    t->priority = kp.sched_priority;
    return 0;
}

static long do_sched_getparam(int pid, struct sched_param_k *param) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    if (!user_ok((uint64_t)param, sizeof(struct sched_param_k))) return -EFAULT;
    struct sched_param_k kparam;
    kparam.sched_priority = t->priority;
    kmemcpy(param, &kparam, sizeof(kparam));
    return 0;
}

/* ── SYS_pipe2 (293) ─────────────────────────────── */

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX      32

struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    int read_pos, write_pos, count;
    int read_open, write_open;
    spinlock_t lock;
};

static struct pipe pipe_pool[PIPE_MAX];
static slab_t pipe_slab;
static int pipe_slab_inited;

static void pipe_slab_ensure(void) {
    if (!pipe_slab_inited) {
        extern void slab_init(slab_t *, void *, int, int);
        slab_init(&pipe_slab, pipe_pool, (int)sizeof(struct pipe), PIPE_MAX);
        pipe_slab_inited = 1;
    }
}

static long pipe_read(struct pipe *pp, void *buf, size_t count) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (pp->count == 0) {
        int wr_open = pp->write_open;
        spin_unlock_irq(&pp->lock, flags);
        return wr_open ? (long)-EAGAIN : 0; /* EOF if write end closed */
    }
    size_t n = count > (size_t)pp->count ? (size_t)pp->count : count;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        dst[i] = pp->buf[pp->read_pos];
        pp->read_pos = (pp->read_pos + 1) % PIPE_BUF_SIZE;
    }
    pp->count -= (int)n;
    spin_unlock_irq(&pp->lock, flags);
    return (long)n;
}

static long pipe_write(struct pipe *pp, const void *buf, size_t count) {
    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (!pp->read_open) {
        spin_unlock_irq(&pp->lock, flags);
        return -EPIPE;
    }
    size_t space = (size_t)(PIPE_BUF_SIZE - pp->count);
    size_t n = count > space ? space : count;
    if (n == 0) {
        spin_unlock_irq(&pp->lock, flags);
        return (long)-EAGAIN;
    }
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        pp->buf[pp->write_pos] = src[i];
        pp->write_pos = (pp->write_pos + 1) % PIPE_BUF_SIZE;
    }
    pp->count += (int)n;
    spin_unlock_irq(&pp->lock, flags);
    return (long)n;
}

static long do_pipe2(int *fds, int flags) {
    (void)flags;
    if (!user_ok((uint64_t)fds, 2 * sizeof(int))) return -EFAULT;

    pipe_slab_ensure();
    struct pipe *pp = (struct pipe *)slab_alloc(&pipe_slab);
    if (!pp) return -ENOMEM;

    pp->read_pos = pp->write_pos = pp->count = 0;
    pp->read_open = pp->write_open = 1;
    pp->lock = (spinlock_t)SPINLOCK_INIT;

    process_t *p = proc_current();
    if (!p) { slab_free(&pipe_slab, pp); return -EFAULT; }

    int rfd = fd_alloc(&p->fds, FD_PIPE, pp, O_RDONLY);
    if (rfd < 0) { slab_free(&pipe_slab, pp); return -EMFILE; }
    int wfd = fd_alloc(&p->fds, FD_PIPE, (void *)((uint8_t *)pp + 1), O_WRONLY);
    if (wfd < 0) {
        fd_close(&p->fds, rfd);
        slab_free(&pipe_slab, pp);
        return -EMFILE;
    }
    /* Mark write-end fd: we encode read/write via pointer offset.
     * Read end: obj == pp. Write end: obj == pp+1 (non-aligned marker). */

    {
        int kfds[2] = { rfd, wfd };
        kmemcpy(fds, kfds, sizeof(kfds));
    }
    return 0;
}

/* Helper: get pipe struct + is_write from fd */
static struct pipe *pipe_from_fd(fd_entry_t *fde, int *is_write) {
    if (!fde || fde->type != FD_PIPE || !fde->obj) return 0;
    /* Read end: obj is aligned to struct pipe. Write end: obj = pp + 1 byte */
    uintptr_t addr = (uintptr_t)fde->obj;
    /* Check if addr is within pipe_pool + offset 1 (write end) */
    uintptr_t base = (uintptr_t)pipe_pool;
    uintptr_t end = base + sizeof(pipe_pool);
    if (addr >= base && addr < end) {
        uintptr_t off = (addr - base) % sizeof(struct pipe);
        if (off == 0) {
            *is_write = 0;
            return (struct pipe *)addr;
        } else if (off == 1) {
            *is_write = 1;
            return (struct pipe *)(addr - 1);
        }
    }
    return 0;
}

static long pipe_close(fd_entry_t *fde) {
    int is_write = 0;
    struct pipe *pp = pipe_from_fd(fde, &is_write);
    if (!pp) return -EBADF;

    uint64_t flags;
    spin_lock_irq(&pp->lock, &flags);
    if (is_write) pp->write_open = 0;
    else pp->read_open = 0;
    int both_closed = !pp->read_open && !pp->write_open;
    spin_unlock_irq(&pp->lock, flags);

    if (both_closed)
        slab_free(&pipe_slab, pp);
    return 0;
}

/* ── fd_cleanup_entry — process-exit cleanup for non-file FDs ── */

void fd_cleanup_entry(int fde_type, void *fde_obj) {
    if (!fde_obj) return;
    if (fde_type == FD_SOCKET) {
        socket_t *s = (socket_t *)fde_obj;
        if (s->state == SOCK_CONNECTED)
            net_tcp_close(&s->tcp);
        s->state = SOCK_UNUSED;
    } else if (fde_type == FD_PIPE) {
        /* Decode read/write end from pointer (write end = pp + 1 byte) */
        uintptr_t addr = (uintptr_t)fde_obj;
        uintptr_t base = (uintptr_t)pipe_pool;
        uintptr_t end = base + sizeof(pipe_pool);
        if (addr >= base && addr < end) {
            uintptr_t off = (addr - base) % sizeof(struct pipe);
            struct pipe *pp = (off <= 1) ? (struct pipe *)(addr - off) : 0;
            if (pp) {
                uint64_t flags;
                spin_lock_irq(&pp->lock, &flags);
                if (off == 0) pp->read_open = 0;
                else          pp->write_open = 0;
                int both_closed = !pp->read_open && !pp->write_open;
                spin_unlock_irq(&pp->lock, flags);
                if (both_closed)
                    slab_free(&pipe_slab, pp);
            }
        }
    } else if (fde_type == FD_EPOLL) {
        epoll_destroy(fde_obj);
    } else if (fde_type == FD_EVENTFD) {
        eventfd_destroy(fde_obj);
    } else if (fde_type == FD_TIMERFD) {
        timerfd_destroy(fde_obj);
    } else if (fde_type == FD_INOTIFY) {
        inotify_destroy(fde_obj);
    }
}

/* ── fd_poll_readiness — check what events are ready on an FD ── */

uint32_t fd_poll_readiness(int fd, uint32_t interest) {
    process_t *p = proc_current();
    if (!p) return EPOLLHUP | EPOLLERR;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type == FD_NONE) return EPOLLHUP | EPOLLERR;

    uint32_t ready = 0;

    switch (fde->type) {
    case FD_SERIAL:
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;

    case FD_SOCKET: {
        socket_t *s = (socket_t *)fde->obj;
        if (!s) { ready |= EPOLLERR; break; }
        if ((interest & EPOLLIN) && s->state == SOCK_CONNECTED) {
            extern pkt_queue_t q_tcp;
            if (s->tcp.rxbuf_pos < s->tcp.rxbuf_len || q_tcp.count > 0)
                ready |= EPOLLIN;
        }
        if ((interest & EPOLLOUT) && s->state == SOCK_CONNECTED)
            ready |= EPOLLOUT;
        break;
    }

    case FD_PIPE: {
        int is_write = 0;
        struct pipe *pp = pipe_from_fd(fde, &is_write);
        if (!pp) { ready |= EPOLLERR; break; }
        if (!is_write) {
            if (interest & EPOLLIN) {
                if (pp->count > 0) ready |= EPOLLIN;
                if (!pp->write_open) ready |= EPOLLIN | EPOLLHUP;
            }
        } else {
            if (interest & EPOLLOUT) {
                if (pp->count < PIPE_BUF_SIZE) ready |= EPOLLOUT;
                if (!pp->read_open) ready |= EPOLLERR | EPOLLHUP;
            }
        }
        break;
    }

    case FD_EVENTFD: {
        eventfd_t *efd = (eventfd_t *)fde->obj;
        if (!efd) { ready |= EPOLLERR; break; }
        if ((interest & EPOLLIN) && efd->counter > 0) ready |= EPOLLIN;
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;
    }

    case FD_TIMERFD: {
        timerfd_t *tfd = (timerfd_t *)fde->obj;
        if (!tfd) { ready |= EPOLLERR; break; }
        /* Check for expired timer */
        if (tfd->armed && timer_ms() >= tfd->expire_ms) {
            uint64_t irqf;
            spin_lock_irq(&tfd->lock, &irqf);
            while (tfd->armed && timer_ms() >= tfd->expire_ms) {
                tfd->expirations++;
                if (tfd->interval_ms > 0)
                    tfd->expire_ms += tfd->interval_ms;
                else
                    tfd->armed = 0;
            }
            spin_unlock_irq(&tfd->lock, irqf);
        }
        if ((interest & EPOLLIN) && tfd->expirations > 0) ready |= EPOLLIN;
        break;
    }

    case FD_FILE:
        if (interest & EPOLLIN)  ready |= EPOLLIN;
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;

    default:
        if (interest & EPOLLOUT) ready |= EPOLLOUT;
        break;
    }

    return ready;
}

/* ── SYS_readv (19) ──────────────────────────────── */

static long do_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    if (!user_ok((uint64_t)iov, (size_t)iovcnt * sizeof(struct iovec))) return -EFAULT;
    /* Copy iov to kernel to prevent TOCTOU — cap at 64 on stack */
    if (iovcnt > 64) iovcnt = 64;
    struct iovec kiov[64];
    kmemcpy(kiov, iov, (size_t)iovcnt * sizeof(struct iovec));
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!user_ok((uint64_t)kiov[i].iov_base, kiov[i].iov_len)) return -EFAULT;
        long r = do_read(fd, (void *)kiov[i].iov_base, kiov[i].iov_len);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((size_t)r < kiov[i].iov_len) break; /* short read */
    }
    return total;
}

/* ── SYS_mkdir/rmdir/unlink/rename ───────────────── */

static long do_mkdir(const char *path, int mode) {
    (void)mode;
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_mkdir(kpath);
}

static long do_mkdirat(int dirfd, const char *path, int mode) {
    (void)dirfd; /* AT_FDCWD only */
    return do_mkdir(path, mode);
}

static long do_rmdir(const char *path) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_rmdir(kpath);
}

static long do_unlink(const char *path) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    return vfs_unlink(kpath);
}

static long do_unlinkat(int dirfd, const char *path, int flags) {
    (void)dirfd;
    if (flags & AT_REMOVEDIR)
        return do_rmdir(path);
    return do_unlink(path);
}

static long do_rename(const char *oldpath, const char *newpath) {
    char kold[PATH_MAX], knew[PATH_MAX];
    int r = copy_path_from_user(kold, oldpath, PATH_MAX);
    if (r < 0) return r;
    r = copy_path_from_user(knew, newpath, PATH_MAX);
    if (r < 0) return r;
    return vfs_rename(kold, knew);
}

static long do_renameat2(int olddirfd, const char *oldpath,
                          int newdirfd, const char *newpath, int flags) {
    (void)olddirfd; (void)newdirfd; (void)flags;
    return do_rename(oldpath, newpath);
}

/* ── SYS_fchmod (91) ─────────────────────────────── */

static long do_fchmod(int fd, uint32_t mode) {
    return vfs_fchmod(fd, mode);
}

/* ── SYS_fchown (93) ─────────────────────────────── */

static long do_fchown(int fd, uint32_t uid, uint32_t gid) {
    return vfs_fchown(fd, uid, gid);
}

/* ── SYS_link (86) ───────────────────────────────── */

static long do_link(const char *oldpath, const char *newpath) {
    char kold[PATH_MAX], knew[PATH_MAX];
    int r = copy_path_from_user(kold, oldpath, PATH_MAX);
    if (r < 0) return r;
    r = copy_path_from_user(knew, newpath, PATH_MAX);
    if (r < 0) return r;
    return vfs_link(kold, knew);
}

/* ── SYS_symlink (88) ───────────────────────────── */

static long do_symlink(const char *target, const char *linkpath) {
    char ktarget[PATH_MAX], klink[PATH_MAX];
    int r = copy_path_from_user(ktarget, target, PATH_MAX);
    if (r < 0) return r;
    r = copy_path_from_user(klink, linkpath, PATH_MAX);
    if (r < 0) return r;
    return vfs_symlink(ktarget, klink);
}

/* ── SYS_readlink (89) ──────────────────────────── */

static long do_readlink(const char *path, char *buf, size_t bufsiz) {
    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    if (!user_ok((uint64_t)buf, bufsiz)) return -EFAULT;
    return vfs_readlink(kpath, buf, bufsiz);
}

/* ── SYS_truncate (76) / SYS_ftruncate (77) ─────── */

static long do_truncate(const char *path, int64_t length) {
    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    return vfs_truncate(kpath, length);
}

static long do_ftruncate(int fd, int64_t length) {
    return vfs_ftruncate(fd, length);
}

/* ── SYS_lstat (6) ──────────────────────────────── */

static long do_lstat(const char *path, struct k_stat *buf) {
    char kpath[PATH_MAX];
    int len = copy_path_from_user(kpath, path, PATH_MAX);
    if (len < 0) return len;
    if (!user_ok((uint64_t)buf, sizeof(struct k_stat))) return -EFAULT;
    return vfs_lstat(kpath, buf);
}

/* ── SYS_fstatat (262) ─────────────────────────── */

static long do_fstatat(int dirfd, const char *path, struct k_stat *buf, int flags) {
    (void)dirfd; /* AT_FDCWD only */
    if (flags & AT_SYMLINK_NOFOLLOW)
        return do_lstat(path, buf);
    return do_stat(path, buf);
}

/* ── SYS_fchmodat (268) ─────────────────────────── */

static long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags) {
    (void)dirfd; (void)flags; /* AT_FDCWD only */
    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;
    return vfs_chmod(kpath, mode);
}

/* ── SYS_utimensat (280) ────────────────────────── */

static long do_utimensat(int dirfd, const char *path, const void *utimes, int flags) {
    (void)dirfd; /* AT_FDCWD only */
    if (!path) return 0; /* futimens with NULL path = no-op for now */

    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;

    int64_t ktimes[4];
    if (utimes) {
        if (!user_ok((uint64_t)utimes, 32)) return -EFAULT;
        kmemcpy(ktimes, utimes, 32); /* 2 × struct timespec = 2 × 16 bytes */
    }

    return vfs_utimensat(kpath, utimes ? ktimes : 0, flags);
}

/* ── SYS_fallocate (285) ────────────────────────── */

static long do_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    if (mode != 0) return -EOPNOTSUPP;
    if (offset < 0 || len <= 0) return -EINVAL;
    int64_t end = offset + len;
    /* Only extend, never shrink — check current size via fstat */
    struct k_stat st;
    int rc = vfs_fstat(fd, &st);
    if (rc < 0) return rc;
    if (end <= st.st_size) return 0;
    return vfs_ftruncate(fd, end);
}

/* ── SYS_mknodat (259) ──────────────────────────── */

static long do_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev) {
    (void)dirfd; (void)dev; /* AT_FDCWD only */

    /* Only S_IFREG (regular files) supported */
    if ((mode & S_IFMT) != S_IFREG && (mode & S_IFMT) != 0)
        return -EPERM;

    char kpath[PATH_MAX];
    int r = copy_path_from_user(kpath, path, PATH_MAX);
    if (r < 0) return r;

    /* Create as regular file via open+close */
    int fd = vfs_open(kpath, O_CREAT | O_WRONLY, (int)mode);
    if (fd < 0) return fd;
    return vfs_close(fd);
}

/* ── SYS_getdents64 (217) ───────────────────────── */

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1]; /* flexible */
};

static long do_getdents64(int fd, void *buf, size_t count) {
    if (!user_ok((uint64_t)buf, count)) return -EFAULT;

    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || !f->node || f->node->type != VFS_DIR) return -ENOTDIR;

    struct vfs_node *dir = f->node;
    uint8_t *out = (uint8_t *)buf;
    size_t written = 0;

    /* Walk to the child at offset f->offset */
    struct vfs_node *child = dir->children;
    uint64_t idx = 0;
    while (child && idx < f->offset) {
        child = child->next;
        idx++;
    }

    while (child) {
        int nlen = 0;
        while (child->name[nlen]) nlen++;
        /* d_reclen: header (19 bytes) + name + NUL, rounded up to 8 */
        size_t reclen = (19 + (size_t)nlen + 1 + 7) & ~(size_t)7;
        if (written + reclen > count) break;

        struct linux_dirent64 *ent = (struct linux_dirent64 *)(out + written);
        ent->d_ino = child->ino;
        ent->d_off = (int64_t)(f->offset + 1);
        ent->d_reclen = (uint16_t)reclen;
        ent->d_type = (child->type == VFS_DIR) ? 4 : 8; /* DT_DIR / DT_REG */
        for (int i = 0; i < nlen; i++)
            ((char *)ent + 19)[i] = child->name[i];
        ((char *)ent + 19)[nlen] = 0;
        /* Zero padding */
        for (size_t i = 19 + (size_t)nlen + 1; i < reclen; i++)
            ((uint8_t *)ent)[i] = 0;

        written += reclen;
        f->offset++;
        child = child->next;
    }

    return (long)written;
}

/* ── SYS_ioctl (16) / SYS_fcntl (72) ────────────── */

#define TIOCGWINSZ 0x5413
#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4

struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

static long do_ioctl(int fd, unsigned long request, unsigned long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    if (request == TIOCGWINSZ) {
        if (!user_ok(arg, sizeof(struct winsize))) return -EFAULT;
        struct winsize *ws = (struct winsize *)arg;
        ws->ws_row = (uint16_t)vt_rows();
        ws->ws_col = (uint16_t)vt_cols();
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    return -ENOTTY;
}

static long do_fcntl(int fd, int cmd, long arg) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    switch (cmd) {
    case F_GETFL: return fde->flags;
    case F_SETFL: fde->flags = (int)arg; return 0;
    case F_GETFD: return (fde->flags & O_CLOEXEC) ? 1 : 0;
    case F_SETFD: {
        if (arg & 1) fde->flags |= O_CLOEXEC;
        else fde->flags &= ~O_CLOEXEC;
        return 0;
    }
    case F_DUPFD: {
        /* Find lowest fd >= arg */
        for (int i = (int)arg; i < FD_MAX; i++) {
            if (!fd_get(&p->fds, i) || p->fds.entries[i].type == FD_NONE) {
                p->fds.entries[i] = *fde;
                if (i >= p->fds.max_fd) p->fds.max_fd = i + 1;
                /* Increment refcount for vfs_file if needed */
                if (fde->type == FD_FILE && fde->obj) {
                    extern void vfs_file_incref(struct vfs_file *f);
                    vfs_file_incref((struct vfs_file *)fde->obj);
                }
                return i;
            }
        }
        return -EMFILE;
    }
    default: return -EINVAL;
    }
}

/* ── SYS_sysinfo (99) ────────────────────────────── */

struct k_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;       /* alignment */
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int  mem_unit;
};

static long do_sysinfo(struct k_sysinfo *info) {
    if (!user_ok((uint64_t)info, sizeof(struct k_sysinfo))) return -EFAULT;
    struct k_sysinfo ksi;
    kmemset(&ksi, 0, sizeof(ksi));
    ksi.uptime = (long)(timer_ms() / 1000);
    ksi.totalram = (unsigned long)page_alloc_total() * 4096;
    ksi.freeram  = (unsigned long)page_alloc_free()  * 4096;
    /* Count live processes */
    unsigned short nprocs = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_pool[i].state == PROC_ALIVE) nprocs++;
    ksi.procs = nprocs;
    ksi.mem_unit = 1;
    kmemcpy(info, &ksi, sizeof(ksi));
    return 0;
}

/* ── SYS_getrusage (98) ─────────────────────────── */

struct k_timeval_ru { long tv_sec; long tv_usec; };

struct k_rusage {
    struct k_timeval_ru ru_utime;
    struct k_timeval_ru ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

static long do_getrusage(int who, struct k_rusage *usage) {
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) return -EINVAL;
    if (!user_ok((uint64_t)usage, sizeof(struct k_rusage))) return -EFAULT;
    struct k_rusage kru;
    kmemset(&kru, 0, sizeof(kru));
    kmemcpy(usage, &kru, sizeof(kru));
    return 0;
}

/* ── SYS_prlimit64 (302) ────────────────────────── */

struct k_rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define RLIM_INFINITY  (~0UL)

static long do_prlimit64(int pid, int resource,
                         const struct k_rlimit *new_rlim,
                         struct k_rlimit *old_rlim) {
    (void)pid; (void)new_rlim; /* ignore set for now */
    if (old_rlim) {
        if (!user_ok((uint64_t)old_rlim, sizeof(struct k_rlimit))) return -EFAULT;
        struct k_rlimit krl;
        switch (resource) {
        case RLIMIT_STACK:
            krl.rlim_cur = 8 * 1024 * 1024;      /* 8 MB */
            krl.rlim_max = 64 * 1024 * 1024;     /* 64 MB */
            break;
        case RLIMIT_NOFILE:
            krl.rlim_cur = FD_MAX;
            krl.rlim_max = FD_MAX;
            break;
        case RLIMIT_AS:
            krl.rlim_cur = RLIM_INFINITY;
            krl.rlim_max = RLIM_INFINITY;
            break;
        default:
            krl.rlim_cur = RLIM_INFINITY;
            krl.rlim_max = RLIM_INFINITY;
            break;
        }
        kmemcpy(old_rlim, &krl, sizeof(krl));
    }
    return 0;
}

/* ── SYS_times (100) ────────────────────────────── */

struct k_tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

static long do_times(struct k_tms *buf) {
    if (!user_ok((uint64_t)buf, sizeof(struct k_tms))) return -EFAULT;
    struct k_tms ktms;
    kmemset(&ktms, 0, sizeof(ktms));
    kmemcpy(buf, &ktms, sizeof(ktms));
    /* Return clock ticks since boot (assume 100 Hz CLK_TCK) */
    return (long)(timer_ms() / 10);
}

/* ── Dispatcher ──────────────────────────────────── */

static long sys_dispatch(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
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
    case SYS_CLONE:         return do_clone((unsigned long)a1, (void *)a2,
                                            (int *)a3, (int *)a4, (unsigned long)a5);
    case SYS_FORK:          return do_fork();
    case SYS_EXECVE:        return do_execve((const char *)a1,
                                             (char *const *)a2, (char *const *)a3);
    case SYS_WAIT4:         return do_wait4((int)a1, (int *)a2, (int)a3, (void *)a4);

    /* Thread/TLS */
    case SYS_ARCH_PRCTL:    return do_arch_prctl((int)a1, (unsigned long)a2);
    case SYS_SET_TID_ADDRESS: {
        thread_t *t = thread_current();
        return t ? (long)t->tid : 1;
    }
    case SYS_SET_ROBUST_LIST: return 0;

    /* Signals */
    case SYS_RT_SIGACTION:    return do_rt_sigaction((int)a1,
                                       (const struct k_sigaction *)a2,
                                       (struct k_sigaction *)a3, (size_t)a4);
    case SYS_RT_SIGPROCMASK:  return do_rt_sigprocmask((int)a1,
                                       (const uint64_t *)a2, (uint64_t *)a3, (size_t)a4);
    case SYS_RT_SIGRETURN:    return do_rt_sigreturn();
    case SYS_KILL:            return do_kill((int)a1, (int)a2);

    /* Identity */
    case SYS_GETPID:  { process_t *p = proc_current(); return p ? (long)p->pid : 1; }
    case SYS_GETPPID: { process_t *p = proc_current(); return p ? (long)p->parent_pid : 0; }
    case SYS_GETTID:  { thread_t *t = thread_current(); return t ? (long)t->tid : 1; }
    case SYS_GETUID:  return 0;
    case SYS_GETGID:  return 0;
    case SYS_GETEUID: return 0;
    case SYS_GETEGID: return 0;

    /* System info */
    case SYS_UNAME:     return do_uname((struct utsname *)a1);
    case SYS_GETRANDOM: return do_getrandom((void *)a1, (size_t)a2, (unsigned int)a3);
    case SYS_PRLIMIT64: return do_prlimit64((int)a1, (int)a2,
                                           (const struct k_rlimit *)a3,
                                           (struct k_rlimit *)a4);
    case SYS_SYSINFO:   return do_sysinfo((struct k_sysinfo *)a1);
    case SYS_GETRUSAGE: return do_getrusage((int)a1, (struct k_rusage *)a2);
    case SYS_TIMES:     return do_times((struct k_tms *)a1);
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
    case SYS_FUTEX:
        if (!user_ok((uint64_t)a1, 4)) return -EFAULT;
        return do_futex((uint32_t *)a1, (int)a2, (uint32_t)a3,
                                        (const struct timespec *)a4,
                                        (uint32_t *)a5, (uint32_t)a6);

    /* Filesystem */
    case SYS_OPEN:   return do_open((const char *)a1, (int)a2, (int)a3);
    case SYS_OPENAT: return do_openat((int)a1, (const char *)a2, (int)a3, (int)a4);
    case SYS_LSEEK:  return do_lseek((int)a1, a2, (int)a3);
    case SYS_FSTAT:  return do_fstat((int)a1, (struct k_stat *)a2);
    case SYS_STAT:   return do_stat((const char *)a1, (struct k_stat *)a2);
    case SYS_LSTAT:  return do_lstat((const char *)a1, (struct k_stat *)a2);
    case SYS_FSTATAT: return do_fstatat((int)a1, (const char *)a2,
                                         (struct k_stat *)a3, (int)a4);
    case SYS_DUP2:   return do_dup2((int)a1, (int)a2);
    case SYS_DUP3:   return do_dup3((int)a1, (int)a2, (int)a3);
    case SYS_GETCWD: return do_getcwd((char *)a1, (size_t)a2);
    case SYS_CHDIR:  return do_chdir((const char *)a1);

    /* Network / sockets */
    case SYS_SOCKET:      return do_socket((int)a1, (int)a2, (int)a3);
    case SYS_CONNECT:     return do_connect((int)a1, (const void *)a2, (int)a3);
    case SYS_BIND:        return do_bind((int)a1, (const void *)a2, (int)a3);
    case SYS_LISTEN:      return do_listen((int)a1, (int)a2);
    case SYS_ACCEPT:      return do_accept((int)a1, (void *)a2, (int *)a3);
    case SYS_SENDTO:      return do_sendto((int)a1, (const void *)a2, a3, (int)a4,
                                           (const void *)a5, (int)a6);
    case SYS_RECVFROM:    return do_recvfrom((int)a1, (void *)a2, a3, (int)a4,
                                             (void *)a5, (int *)a6);
    case SYS_SETSOCKOPT:  return do_setsockopt((int)a1, (int)a2, (int)a3,
                                               (const void *)a4, (int)a5);
    case SYS_GETSOCKOPT:  return do_getsockopt((int)a1, (int)a2, (int)a3,
                                               (void *)a4, (int *)a5);
    case SYS_GETSOCKNAME: return do_getsockname((int)a1, (void *)a2, (int *)a3);
    case SYS_GETPEERNAME: return do_getpeername((int)a1, (void *)a2, (int *)a3);
    case SYS_SENDMSG:     return -ENOSYS;
    case SYS_RECVMSG:     return -ENOSYS;
    case SYS_SHUTDOWN:     return 0;
    case SYS_SOCKETPAIR:  return -ENOSYS;
    case SYS_POLL:        return do_poll((void *)a1, (int)a2, (int)a3);

    /* Filesystem mutation */
    case SYS_MKDIR:      return do_mkdir((const char *)a1, (int)a2);
    case SYS_MKDIRAT:    return do_mkdirat((int)a1, (const char *)a2, (int)a3);
    case SYS_RMDIR:      return do_rmdir((const char *)a1);
    case SYS_UNLINK:     return do_unlink((const char *)a1);
    case SYS_UNLINKAT:   return do_unlinkat((int)a1, (const char *)a2, (int)a3);
    case SYS_RENAME:     return do_rename((const char *)a1, (const char *)a2);
    case SYS_RENAMEAT2:  return do_renameat2((int)a1, (const char *)a2,
                                              (int)a3, (const char *)a4, (int)a5);
    case SYS_GETDENTS64: return do_getdents64((int)a1, (void *)a2, (size_t)a3);

    /* Filesystem metadata */
    case SYS_FCHMOD:     return do_fchmod((int)a1, (uint32_t)a2);
    case SYS_FCHOWN:     return do_fchown((int)a1, (uint32_t)a2, (uint32_t)a3);
    case SYS_LINK:       return do_link((const char *)a1, (const char *)a2);
    case SYS_SYMLINK:    return do_symlink((const char *)a1, (const char *)a2);
    case SYS_READLINK:   return do_readlink((const char *)a1, (char *)a2, (size_t)a3);
    case SYS_TRUNCATE:   return do_truncate((const char *)a1, (int64_t)a2);
    case SYS_FTRUNCATE:  return do_ftruncate((int)a1, (int64_t)a2);
    case SYS_FCHMODAT:   return do_fchmodat((int)a1, (const char *)a2, (uint32_t)a3, (int)a4);
    case SYS_UTIMENSAT:  return do_utimensat((int)a1, (const char *)a2, (const void *)a3, (int)a4);
    case SYS_FALLOCATE:  return do_fallocate((int)a1, (int)a2, (int64_t)a3, (int64_t)a4);
    case SYS_MKNODAT:    return do_mknodat((int)a1, (const char *)a2, (uint32_t)a3, (uint64_t)a4);

    /* Pipe / IO */
    case SYS_PIPE:   return do_pipe2((int *)a1, 0);
    case SYS_PIPE2:  return do_pipe2((int *)a1, (int)a2);
    case SYS_READV:  return do_readv((int)a1, (const struct iovec *)a2, (int)a3);
    case SYS_IOCTL:  return do_ioctl((int)a1, (unsigned long)a2, (unsigned long)a3);
    case SYS_FCNTL:  return do_fcntl((int)a1, (int)a2, a3);

    /* Stubs */
    case SYS_ACCESS: return 0; /* pretend everything is accessible */

    /* epoll / eventfd / timerfd / signalfd / inotify */
    case SYS_EPOLL_CREATE1:     return do_epoll_create1((int)a1);
    case SYS_EPOLL_CTL:         return do_epoll_ctl((int)a1, (int)a2, (int)a3,
                                                     (struct epoll_event *)a4);
    case SYS_EPOLL_WAIT:        return do_epoll_wait((int)a1, (struct epoll_event *)a2,
                                                      (int)a3, (int)a4);
    case SYS_EVENTFD2:          return do_eventfd2((unsigned int)a1, (int)a2);
    case SYS_TIMERFD_CREATE:    return do_timerfd_create((int)a1, (int)a2);
    case SYS_TIMERFD_SETTIME:   return do_timerfd_settime((int)a1, (int)a2,
                                         (const struct k_itimerspec *)a3,
                                         (struct k_itimerspec *)a4);
    case SYS_SIGNALFD4:         return do_signalfd4((int)a1, (const uint64_t *)a2, (int)a3);
    case SYS_INOTIFY_INIT1:     return do_inotify_init1((int)a1);
    case SYS_INOTIFY_ADD_WATCH: return do_inotify_add_watch((int)a1, (const char *)a2,
                                                             (uint32_t)a3);
    case SYS_INOTIFY_RM_WATCH:  return do_inotify_rm_watch((int)a1, (int)a2);

    /* ── CosmoRT Hardware Primitives (for userspace drivers) ── */
    /* Capability check: only processes with is_driver may use these */
#define HW_CAP_CHECK() do { \
    process_t *_p = proc_current(); \
    if (!_p || !_p->is_driver) return -EPERM; \
} while (0)

    case SYS_COSMO_MMIO_MAP: {
        HW_CAP_CHECK();
        if (!user_ok(a3, 8)) return -EFAULT;
        void *virt;
        int r = cosmo_mmio_map((uint64_t)a1, (size_t)a2, &virt);
        if (r == 0) *(void **)a3 = virt;
        return r;
    }
    case SYS_COSMO_DMA_ALLOC: {
        HW_CAP_CHECK();
        if (!user_ok(a2, 8) || !user_ok(a3, 8)) return -EFAULT;
        void *virt; uint64_t phys;
        int r = cosmo_dma_alloc((size_t)a1, &virt, &phys);
        if (r == 0) { *(void **)a2 = virt; *(uint64_t *)a3 = phys; }
        return r;
    }
    case SYS_COSMO_DMA_FREE:
        HW_CAP_CHECK();
        cosmo_dma_free((void *)a1, (size_t)a2);
        return 0;
    case SYS_COSMO_IRQ_REGISTER:
        HW_CAP_CHECK();
        return cosmo_irq_register((int)a1, (void (*)(void *))a2, (void *)a3);
    case SYS_COSMO_PCI_READ: {
        HW_CAP_CHECK();
        if (!user_ok(a4, 4)) return -EFAULT;
        return cosmo_pci_config_read((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t *)a5);
    }
    case SYS_COSMO_PCI_WRITE:
        HW_CAP_CHECK();
        return cosmo_pci_config_write((int)a1, (int)a2, (int)a3, (int)a4, (uint32_t)a5);
    case SYS_COSMO_FW_LOAD: {
        HW_CAP_CHECK();
        if (!user_ok(a2, 8) || !user_ok(a3, 8)) return -EFAULT;
        return cosmo_fw_load((const char *)a1, (void **)a2, (size_t *)a3);
    }
    case SYS_COSMO_NIC_ATTACH: {
        HW_CAP_CHECK();
        /* a1 = ptr to { uint64_t shm_phys; uint64_t shm_size; uint8_t mac[6]; } */
        if (!user_ok(a1, 22)) return -EFAULT;
        struct { uint64_t shm_phys; uint64_t shm_size; uint8_t mac[6]; } kargs;
        kmemcpy(&kargs, (const void *)a1, sizeof(kargs));
        return net_port_attach(kargs.shm_phys, (size_t)kargs.shm_size, kargs.mac);
    }

    case SYS_COSMO_KEXEC: {
        HW_CAP_CHECK();
        if (!user_ok(a1, (size_t)a2)) return -EFAULT;
        extern int do_kexec(const void *, size_t);
        return do_kexec((const void *)a1, (size_t)a2);
    }
#undef HW_CAP_CHECK

    default:
        serial_puts("syscall: unhandled #");
        serial_hex64((uint64_t)num);
        serial_putchar('\n');
        return -ENOSYS;
    }
}

long sys_handler(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long result = sys_dispatch(num, a1, a2, a3, a4, a5, a6);
    check_signals_syscall_path(&result, num);
    return result;
}

/* Check and deliver signals in the SYSCALL return path.
 * Syncs percpu syscall frame ↔ thread_t around delivery.
 * Called from syscall_entry.asm between sys_handler return and SYSRET.
 * Actually called from the ASM-adjacent C code. */
void check_signals_syscall_path(long *result_ptr, long num) {
    if (num == SYS_RT_SIGRETURN) return;
    thread_t *t = thread_current();
    if (!t || !t->proc) return;
    process_t *p = t->proc;
    uint64_t deliverable = p->sig_pending & ~p->sig_blocked;
    if (!deliverable) return;

    percpu_t *cpu = percpu_self();
    syscall_frame_t *frame = (syscall_frame_t *)cpu->syscall_frame;
    /* Save syscall frame → thread_t */
    t->rip    = frame->rcx;
    t->rflags = frame->r11;
    t->rsp    = cpu->user_rsp;
    t->rax    = (uint64_t)*result_ptr;
    t->rbx = frame->rbx; t->rdx = frame->rdx;
    t->rsi = frame->rsi; t->rdi = frame->rdi; t->rbp = frame->rbp;
    t->r8  = frame->r8;  t->r9  = frame->r9;  t->r10 = frame->r10;
    t->r12 = frame->r12; t->r13 = frame->r13;
    t->r14 = frame->r14; t->r15 = frame->r15;

    check_pending_signals();

    /* Write back thread_t → syscall frame */
    frame->rcx = t->rip;
    frame->r11 = t->rflags;
    cpu->user_rsp = t->rsp;
    *result_ptr = (long)t->rax;
    frame->rbx = t->rbx; frame->rdx = t->rdx;
    frame->rsi = t->rsi; frame->rdi = t->rdi; frame->rbp = t->rbp;
    frame->r8  = t->r8;  frame->r9  = t->r9;  frame->r10 = t->r10;
    frame->r12 = t->r12; frame->r13 = t->r13;
    frame->r14 = t->r14; frame->r15 = t->r15;
}
