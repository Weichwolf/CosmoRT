/* CosmoRT procfs — virtual filesystem at /proc
 *
 * Read-only. Entries registered with callbacks that generate content on read.
 * Max 32 entries, static allocation, no dynamic memory.
 */

#include "fs/procfs.h"
#include "hw/serial.h"
#include "mm/page_alloc.h"
#include "core/smp.h"
#include "core/timer.h"
#include "memops.h"
#include "config.h"
#include "proc/proc_internal.h"
#include "mm/vma.h"
#include "linux/errno.h"
#include "core/time_ns.h"

#define PROCFS_HUGE_PAGE (2ULL * 1024 * 1024)

/* ── Entry table ─────────────────────────────────── */

#define PROCFS_MAX 48

typedef struct {
    char name[64];
    procfs_read_fn  fn;
    procfs_write_fn wfn;
    void *ctx;
} procfs_entry_t;

static procfs_entry_t entries[PROCFS_MAX];
static int num_entries;

void procfs_register_rw(const char *name, procfs_read_fn rfn,
                        procfs_write_fn wfn, void *ctx) {
    if (num_entries >= PROCFS_MAX) return;
    procfs_entry_t *e = &entries[num_entries++];
    int i = 0;
    while (name[i] && i < 63) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->fn  = rfn;
    e->wfn = wfn;
    e->ctx = ctx;
}

void procfs_register(const char *name, procfs_read_fn fn, void *ctx) {
    procfs_register_rw(name, fn, 0, ctx);
}

static int find_entry(const char *name) {
    for (int i = 0; i < num_entries; i++) {
        const char *a = entries[i].name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

/* ── VFS hooks ───────────────────────────────────── */

int procfs_open(const char *name) {
    int idx = find_entry(name);
    if (idx < 0) return 0;
    return idx + 1; /* handle = index + 1 (0 = failure) */
}

int procfs_read(int handle, char *buf, int size, int offset) {
    if (handle < 1 || handle > num_entries) return 0;
    procfs_entry_t *e = &entries[handle - 1];
    if (!e->fn) return 0;
    return e->fn(buf, size, offset, e->ctx);
}

long procfs_write(int handle, const char *buf, int size, int offset) {
    if (handle < 1 || handle > num_entries) return -EBADF;
    procfs_entry_t *e = &entries[handle - 1];
    if (!e->wfn) return -EACCES;
    return e->wfn(buf, size, offset, e->ctx);
}

int procfs_writable(const char *name) {
    int idx = find_entry(name);
    if (idx < 0) return 0;
    return entries[idx].wfn ? 1 : 0;
}

void procfs_close(int handle) {
    (void)handle; /* nothing to free */
}

int procfs_stat(const char *name, int *size_out) {
    int idx = find_entry(name);
    if (idx < 0) return -1;
    /* For procfs files, report 0 size (content generated on read).
     * Callers that need the actual size should read to determine it. */
    if (size_out) *size_out = 0;
    return 0;
}

/* ── Iterate entries (for getdents64 on /proc) ───── */

int procfs_iterate(int offset, int (*cb)(const char *name, void *ctx), void *ctx) {
    int visited = 0;
    for (int i = offset; i < num_entries; i++) {
        visited++;
        if (cb(entries[i].name, ctx)) break;
    }
    return visited;
}

/* ── int-to-string helper ────────────────────────── */

static int itoa_buf(char *dst, int max, long v) {
    char tmp[20];
    int neg = 0, i = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { tmp[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    int len = i + neg;
    if (len > max) return 0;
    int pos = 0;
    if (neg) dst[pos++] = '-';
    while (i--) dst[pos++] = tmp[i];
    return len;
}

static int append_str(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max) buf[pos++] = *s++;
    return pos;
}

static int append_int(char *buf, int pos, int max, long v) {
    char tmp[20];
    int n = itoa_buf(tmp, 20, v);
    for (int i = 0; i < n && pos < max; i++) buf[pos++] = tmp[i];
    return pos;
}

/* Right-align an integer field to at least `width` chars */
static int append_int_rpad(char *buf, int pos, int max, long v, int width) {
    char tmp[20];
    int n = itoa_buf(tmp, 20, v);
    for (int i = 0; i < width - n && pos < max; i++) buf[pos++] = ' ';
    for (int i = 0; i < n && pos < max; i++) buf[pos++] = tmp[i];
    return pos;
}

/* ── Built-in entries ────────────────────────────── */

static int procfs_dmesg(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    return serial_dmesg_read(buf, offset, size);
}

static int procfs_meminfo(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    int total = page_alloc_total();
    int free_pg = page_alloc_free();
    long total_kb = (long)total * 4;
    long free_kb  = (long)free_pg * 4;

    /* Linux-compatible format: right-aligned values with spaces */
    char tmp[512];
    int pos = 0;
    pos = append_str(tmp, pos, 512, "MemTotal:");
    pos = append_int_rpad(tmp, pos, 512, total_kb, 16);
    pos = append_str(tmp, pos, 512, " kB\nMemFree:");
    pos = append_int_rpad(tmp, pos, 512, free_kb, 17);
    pos = append_str(tmp, pos, 512, " kB\nMemAvailable:");
    pos = append_int_rpad(tmp, pos, 512, free_kb, 12);
    pos = append_str(tmp, pos, 512, " kB\nBuffers:");
    pos = append_int_rpad(tmp, pos, 512, 0, 17);
    pos = append_str(tmp, pos, 512, " kB\nCached:");
    pos = append_int_rpad(tmp, pos, 512, 0, 18);
    pos = append_str(tmp, pos, 512, " kB\n");

    if (offset >= pos) return 0;
    int avail = pos - offset;
    if (size > avail) size = avail;
    for (int i = 0; i < size; i++) buf[i] = tmp[offset + i];
    return size;
}

static int procfs_cpuinfo(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    int cores = smp_num_cores();
    uint64_t tsc_khz = timer_tsc_per_ms;
    long mhz = (long)(tsc_khz / 1000);

    /* Linux-compatible format, one block per core */
    char tmp[1024];
    int pos = 0;
    for (int i = 0; i < cores && pos < 900; i++) {
        if (i > 0) pos = append_str(tmp, pos, 1024, "\n");
        pos = append_str(tmp, pos, 1024, "processor\t: ");
        pos = append_int(tmp, pos, 1024, (long)i);
        pos = append_str(tmp, pos, 1024, "\nvendor_id\t: CosmoRT");
        pos = append_str(tmp, pos, 1024, "\nmodel name\t: CosmoRT vCPU");
        pos = append_str(tmp, pos, 1024, "\ncpu MHz\t\t: ");
        pos = append_int(tmp, pos, 1024, mhz);
        pos = append_str(tmp, pos, 1024, "\n");
    }

    if (offset >= pos) return 0;
    int avail = pos - offset;
    if (size > avail) size = avail;
    for (int i = 0; i < size; i++) buf[i] = tmp[offset + i];
    return size;
}

/* ── /proc/self/maps ────────────────────────────── */

#include "mm/vma.h"
#include "proc/process.h"

/* Hex helper: append 64-bit hex (no leading zeros stripped — always 12 hex chars for compactness) */
static int append_hex(char *buf, int pos, int max, uint64_t v) {
    char tmp[16];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        /* Find number of hex digits needed */
        uint64_t t = v;
        int digits = 0;
        while (t) { digits++; t >>= 4; }
        n = digits;
        for (int i = digits - 1; i >= 0; i--) {
            tmp[i] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        }
    }
    for (int i = 0; i < n && pos < max; i++) buf[pos++] = tmp[i];
    return pos;
}

/* Context for VMA walk */
struct maps_ctx {
    char *buf;
    int max;    /* buffer size */
    int total;  /* total generated bytes (may exceed max) */
    int offset; /* requested read offset */
    int written;/* bytes actually written to buf */
    uint64_t stack_top; /* process stack top (for [stack] label) */
    uint64_t brk_base;  /* brk base (for [heap] label) */
    uint64_t brk_cur;   /* brk current (for [heap] label) */
};

static void maps_append(struct maps_ctx *c, const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (c->total >= c->offset && c->written < c->max)
            c->buf[c->written++] = s[i];
        c->total++;
    }
}

static void vma_walk_maps(vma_t *node, struct maps_ctx *c) {
    if (!node) return;
    vma_walk_maps(node->left, c);

    /* Linux format: <start>-<end> <perms> <offset> <dev> <inode> <pathname>\n
     * Example: 7ffffffde000-7ffffffff000 rw-p 00000000 00:00 0          [stack] */
    char line[256];
    int lp = 0;
    lp = append_hex(line, lp, 200, node->start);
    line[lp++] = '-';
    lp = append_hex(line, lp, 200, node->end);
    line[lp++] = ' ';
    line[lp++] = (node->prot & PROT_READ)  ? 'r' : '-';
    line[lp++] = (node->prot & PROT_WRITE) ? 'w' : '-';
    line[lp++] = (node->prot & PROT_EXEC)  ? 'x' : '-';
    line[lp++] = (node->flags & VMA_SHARED) ? 's' : 'p';
    line[lp++] = ' ';

    /* File offset (hex, 8 digits) */
    {
        uint64_t off = node->file_offset;
        for (int i = 7; i >= 0; i--) {
            line[lp + i] = "0123456789abcdef"[off & 0xf];
            off >>= 4;
        }
        lp += 8;
    }
    line[lp++] = ' ';
    /* dev major:minor — 00:00 (no block device tracking) */
    line[lp++] = '0'; line[lp++] = '0'; line[lp++] = ':';
    line[lp++] = '0'; line[lp++] = '0'; line[lp++] = ' ';
    /* inode */
    if (node->file_ino) {
        lp = append_hex(line, lp, 200, node->file_ino);
    } else {
        line[lp++] = '0';
    }

    /* Label: [stack], [heap], or nothing.
     * Linux labels the stack VMA and heap VMA. */
    const char *label = 0;
    if ((node->flags & VMA_GROWSDOWN) && c->stack_top &&
        node->end <= c->stack_top && node->start < c->stack_top)
        label = "[stack]";
    else if (c->brk_base && node->start >= c->brk_base &&
             node->end <= c->brk_cur + 0x1000)
        label = "[heap]";

    if (label) {
        /* Pad to column 73 (Linux convention) */
        while (lp < 73 && lp < 200) line[lp++] = ' ';
        for (int i = 0; label[i] && lp < 200; i++) line[lp++] = label[i];
    }

    line[lp++] = '\n';
    maps_append(c, line, lp);

    vma_walk_maps(node->right, c);
}

/* ── procfs_fd_t pool (max 32 open procfs fds) ───── */

#define PROCFS_FD_MAX 32
static procfs_fd_t fd_pool[PROCFS_FD_MAX];
static int fd_used[PROCFS_FD_MAX];

procfs_fd_t *procfs_fd_alloc(void) {
    for (int i = 0; i < PROCFS_FD_MAX; i++) {
        if (!fd_used[i]) {
            fd_used[i] = 1;
            fd_pool[i].handle = 0;
            fd_pool[i].offset = 0;
            return &fd_pool[i];
        }
    }
    return 0;
}

void procfs_fd_free(procfs_fd_t *pf) {
    if (!pf) return;
    int idx = (int)(pf - fd_pool);
    if (idx >= 0 && idx < PROCFS_FD_MAX)
        fd_used[idx] = 0;
}

/* ── /proc/pid/status ──────────────────────────── */

static uint64_t vma_sum_size(vma_t *node) {
    if (!node) return 0;
    return vma_sum_size(node->left)
         + (node->end - node->start)
         + vma_sum_size(node->right);
}

static uint64_t vma_sum_data(vma_t *node) {
    if (!node) return 0;
    uint64_t sum = vma_sum_data(node->left) + vma_sum_data(node->right);
    if ((node->flags & MAP_ANONYMOUS) && (node->prot & PROT_WRITE) &&
        !(node->flags & VMA_GROWSDOWN))
        sum += node->end - node->start;
    return sum;
}

static uint64_t vma_sum_stack(vma_t *node) {
    if (!node) return 0;
    uint64_t sum = vma_sum_stack(node->left) + vma_sum_stack(node->right);
    if (node->flags & VMA_GROWSDOWN)
        sum += node->end - node->start;
    return sum;
}

static uint64_t vma_sum_exe(vma_t *node) {
    if (!node) return 0;
    uint64_t sum = vma_sum_exe(node->left) + vma_sum_exe(node->right);
    if (node->prot & PROT_EXEC) sum += node->end - node->start;
    return sum;
}

static uint64_t proc_count_rss(process_t *p) {
    if (!p->pml4) return 0;
    uint64_t rss = 0;
    uint64_t *pml4 = p->pml4;
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_PS) { rss += PROCFS_HUGE_PAGE; continue; }
                uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++)
                    if (pt[l] & PTE_PRESENT) rss += 4096;
            }
        }
    }
    return rss;
}

/* Linux-konforme State-Ableitung: 'R' running, 'S' interruptible-sleep,
 * 'T' stopped, 'Z' zombie, 'X' dead. Basiert auf main_thread->state, weil
 * PROC_ALIVE alleine nicht zwischen running und blocked unterscheidet. */
static char proc_state_char(const process_t *p) {
    if (p->state == PROC_ZOMBIE) return 'Z';
    thread_t *t = p->main_thread;
    if (!t) return 'R';
    switch (t->state) {
    case THREAD_BLOCKED: return 'S';
    case THREAD_STOPPED: return 'T';
    case THREAD_DEAD:    return 'X';
    default:             return 'R';
    }
}

static const char *proc_state_str(char c) {
    switch (c) {
    case 'Z': return "Z (zombie)";
    case 'S': return "S (sleeping)";
    case 'T': return "T (stopped)";
    case 'X': return "X (dead)";
    default:  return "R (running)";
    }
}

static int procfs_pid_status(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;

    const char *state_str = proc_state_str(proc_state_char(p));

    uint64_t vm_bytes, data_bytes, stk_bytes, exe_bytes, rss_bytes;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    vm_bytes   = vma_sum_size(p->vma_root);
    data_bytes = vma_sum_data(p->vma_root);
    stk_bytes  = vma_sum_stack(p->vma_root);
    exe_bytes  = vma_sum_exe(p->vma_root);
    rss_bytes  = proc_count_rss(p);
    spin_unlock_irq(&p->lock, irqf);

    char tmp[1024];
    int pos = 0;
    pos = append_str(tmp, pos, 1024, "Name:\t");
    pos = append_str(tmp, pos, 1024, p->comm[0] ? p->comm : "?");
    pos = append_str(tmp, pos, 1024, "\nUmask:\t0");
    pos = append_int(tmp, pos, 1024, (long)((p->umask_val >> 6) & 7));
    pos = append_int(tmp, pos, 1024, (long)((p->umask_val >> 3) & 7));
    pos = append_int(tmp, pos, 1024, (long)(p->umask_val & 7));
    pos = append_str(tmp, pos, 1024, "\nState:\t");
    pos = append_str(tmp, pos, 1024, state_str);
    pos = append_str(tmp, pos, 1024, "\nTgid:\t");
    pos = append_int(tmp, pos, 1024, (long)p->pid);
    pos = append_str(tmp, pos, 1024, "\nPid:\t");
    pos = append_int(tmp, pos, 1024, (long)p->pid);
    pos = append_str(tmp, pos, 1024, "\nPPid:\t");
    pos = append_int(tmp, pos, 1024, (long)p->parent_pid);
    pos = append_str(tmp, pos, 1024, "\nTracerPid:\t0");
    pos = append_str(tmp, pos, 1024, "\nUid:\t0\t0\t0\t0");
    pos = append_str(tmp, pos, 1024, "\nGid:\t0\t0\t0\t0");
    pos = append_str(tmp, pos, 1024, "\nFDSize:\t");
    pos = append_int(tmp, pos, 1024, (long)p->fds.max_fd);
    pos = append_str(tmp, pos, 1024, "\nGroups:\t");
    pos = append_str(tmp, pos, 1024, "\nVmPeak:\t");
    pos = append_int(tmp, pos, 1024, (long)(vm_bytes / 1024));
    pos = append_str(tmp, pos, 1024, " kB\nVmSize:\t");
    pos = append_int(tmp, pos, 1024, (long)(vm_bytes / 1024));
    pos = append_str(tmp, pos, 1024, " kB\nVmRSS:\t");
    pos = append_int(tmp, pos, 1024, (long)(rss_bytes / 1024));
    pos = append_str(tmp, pos, 1024, " kB\nVmData:\t");
    pos = append_int(tmp, pos, 1024, (long)(data_bytes / 1024));
    pos = append_str(tmp, pos, 1024, " kB\nVmStk:\t");
    pos = append_int(tmp, pos, 1024, (long)(stk_bytes / 1024));
    pos = append_str(tmp, pos, 1024, " kB\nVmExe:\t");
    pos = append_int(tmp, pos, 1024, (long)(exe_bytes / 1024));
    pos = append_str(tmp, pos, 1024, " kB\nThreads:\t");
    pos = append_int(tmp, pos, 1024, (long)p->thread_count);
    pos = append_str(tmp, pos, 1024, "\nSigPnd:\t0000000000000000");
    pos = append_str(tmp, pos, 1024, "\nSigBlk:\t0000000000000000");
    pos = append_str(tmp, pos, 1024, "\nSigIgn:\t0000000000000000");
    pos = append_str(tmp, pos, 1024, "\nSigCgt:\t0000000000000000\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/self/stat ───────────────────────────────── */

static int procfs_pid_stat(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;

    char state_c = proc_state_char(p);

    uint64_t vsize_b = 0;
    uint64_t rss_pages = 0;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    vsize_b = vma_sum_size(p->vma_root);
    rss_pages = proc_count_rss(p) / 4096;
    spin_unlock_irq(&p->lock, irqf);

    char tmp[512];
    int pos = 0;
    /* 1=pid 2=(comm) 3=state 4=ppid */
    pos = append_int(tmp, pos, 512, (long)p->pid);
    pos = append_str(tmp, pos, 512, " (");
    pos = append_str(tmp, pos, 512, p->comm[0] ? p->comm : "?");
    pos = append_str(tmp, pos, 512, ") ");
    if (pos < 511) tmp[pos++] = state_c;
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)p->parent_pid);
    /* 5=pgrp 6=session */
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)p->pgid);
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)p->sid);
    /* 7..13: tty_nr tpgid flags minflt cminflt majflt cmajflt */
    pos = append_str(tmp, pos, 512, " 0 0 0 0 0 0 0");
    /* 14..17: utime stime cutime cstime (clock ticks).
     * cpu_time_ticks accrues at 1000Hz. Report as utime in USER_HZ=100 units
     * (divide by 10). Split 80/20 user/system to avoid all-zero stime. */
    {
        long uhz = (long)(p->cpu_time_ticks / 10);
        if (uhz == 0 && p->cpu_time_ticks > 0) uhz = 1;
        long utime_tk = (uhz * 8 + 9) / 10;
        long stime_tk = uhz - utime_tk;
        pos = append_str(tmp, pos, 512, " ");
        pos = append_int(tmp, pos, 512, utime_tk);
        pos = append_str(tmp, pos, 512, " ");
        pos = append_int(tmp, pos, 512, stime_tk);
        pos = append_str(tmp, pos, 512, " 0 0");
    }
    /* 18=priority 19=nice 20=num_threads 21=itrealvalue */
    pos = append_str(tmp, pos, 512, " 20 0 ");
    pos = append_int(tmp, pos, 512, (long)p->thread_count);
    pos = append_str(tmp, pos, 512, " 0");
    /* 22=starttime (jiffies since boot) */
    pos = append_str(tmp, pos, 512, " 0");
    /* 23=vsize 24=rss 25=rsslim */
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)vsize_b);
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)rss_pages);
    pos = append_str(tmp, pos, 512, " 18446744073709551615");
    /* 26=startcode 27=endcode 28=startstack 29=kstkesp 30=kstkeip */
    pos = append_str(tmp, pos, 512, " 0 0 ");
    pos = append_int(tmp, pos, 512, (long)p->stack_top);
    pos = append_str(tmp, pos, 512, " 0 0");
    /* 31..34: signal blocked sigignore sigcatch */
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)p->sig_pending);
    pos = append_str(tmp, pos, 512, " 0 0 0");
    /* 35=wchan 36=nswap 37=cnswap 38=exit_signal */
    pos = append_str(tmp, pos, 512, " 0 0 0 ");
    pos = append_int(tmp, pos, 512, (long)(p->notify_signal ? p->notify_signal : 17));
    /* 39=processor 40=rt_priority 41=policy 42=delayacct_blkio_ticks */
    pos = append_str(tmp, pos, 512, " 0 0 0 0");
    /* 43=guest_time 44=cguest_time */
    pos = append_str(tmp, pos, 512, " 0 0");
    /* 45=start_data 46=end_data 47=start_brk */
    pos = append_str(tmp, pos, 512, " 0 0 ");
    pos = append_int(tmp, pos, 512, (long)p->brk_base);
    /* 48..51: arg_start arg_end env_start env_end — not tracked */
    pos = append_str(tmp, pos, 512, " 0 0 0 0");
    /* 52=exit_code */
    pos = append_str(tmp, pos, 512, " ");
    pos = append_int(tmp, pos, 512, (long)p->exit_code);
    pos = append_str(tmp, pos, 512, "\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/sys/vm/overcommit_memory ────────────────── */

static int procfs_overcommit(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    if (offset > 0) return 0;
    if (size < 2) return 0;
    buf[0] = '0'; buf[1] = '\n';
    return 2;
}

/* ── /proc/cmdline — Kernel-Command-Line ───────────── */

static int procfs_cmdline(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "BOOT_IMAGE=/boot/cosmo\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/mounts — Mount-Tabelle ───────────────────
 * Format: <src> <mountpoint> <fstype> <options> <freq> <passno>\n
 * Linux referenziert /proc/self/mounts; /proc/mounts ist historisch
 * ein Symlink auf /proc/self/mounts. */

static int procfs_mounts(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s =
        "rootfs / rootfs rw 0 0\n"
        "tmpfs /tmp tmpfs rw 0 0\n"
        "proc /proc proc rw 0 0\n"
        "sysfs /sys sysfs rw 0 0\n"
        "devfs /dev devfs rw 0 0\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/sys/vm/mmap_min_addr ─────────────────────
 * Linux default: 65536 (x86_64). */

static int procfs_mmap_min_addr(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "65536\n";
    int len = 6;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/self/statm ──────────────────────────────── */

static int procfs_self_statm(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    /* size resident shared text lib data dt (in pages) */
    const char *s = "0 0 0 0 0 0 0\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/self/cgroup ─────────────────────────────── */

static int procfs_self_cgroup(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "0::/\n";
    int len = 5;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/stat — per-CPU times ────────────────────── */

static int procfs_global_stat(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    extern uint64_t timer_ms(void);
    uint64_t ms = timer_ms();
    /* All tracked time as idle — no user/sys accounting yet. 10 Linux fields:
     * user nice system idle iowait irq softirq steal guest guest_nice. */
    long jiffies = (long)(ms / 10);

    char tmp[1024];
    int pos = 0;
    pos = append_str(tmp, pos, 1024, "cpu  0 0 0 ");
    pos = append_int(tmp, pos, 1024, jiffies);
    pos = append_str(tmp, pos, 1024, " 0 0 0 0 0 0\n");
    int ncores = smp_num_cores();
    for (int c = 0; c < ncores && pos < 900; c++) {
        pos = append_str(tmp, pos, 1024, "cpu");
        pos = append_int(tmp, pos, 1024, (long)c);
        pos = append_str(tmp, pos, 1024, " 0 0 0 ");
        pos = append_int(tmp, pos, 1024, jiffies / ncores);
        pos = append_str(tmp, pos, 1024, " 0 0 0 0 0 0\n");
    }
    pos = append_str(tmp, pos, 1024, "intr 0\nctxt 0\nbtime 0\nprocesses ");
    pos = append_int(tmp, pos, 1024, (long)proc_count_alive());
    pos = append_str(tmp, pos, 1024, "\nprocs_running 1\nprocs_blocked 0\nsoftirq 0\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/uptime ──────────────────────────────────── */

static int procfs_uptime(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    extern uint64_t timer_ms(void);
    uint64_t ms = timer_ms();
    long sec = (long)(ms / 1000);
    long frac = (long)((ms % 1000) / 10);

    char tmp[256];
    int pos = 0;
    pos = append_int(tmp, pos, 256, sec);
    pos = append_str(tmp, pos, 256, ".");
    if (frac < 10) pos = append_str(tmp, pos, 256, "0");
    pos = append_int(tmp, pos, 256, frac);
    pos = append_str(tmp, pos, 256, " 0.00\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/loadavg ─────────────────────────────────── */

static int procfs_loadavg(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    int nproc = proc_count_alive();
    process_t *cur = proc_current();
    long last_pid = cur ? (long)cur->pid : 1;

    char tmp[128];
    int pos = 0;
    pos = append_str(tmp, pos, 128, "0.00 0.00 0.00 1/");
    pos = append_int(tmp, pos, 128, (long)nproc);
    pos = append_str(tmp, pos, 128, " ");
    pos = append_int(tmp, pos, 128, last_pid);
    pos = append_str(tmp, pos, 128, "\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/version ─────────────────────────────────── */

static int procfs_version(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "CosmoRT version 0.1.0 (gcc) #1 SMP\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/bus/pci/devices ─────────────────────────── */

#include "cosmort.h"

static int procfs_pci_devices(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char tmp[2048];
    int pos = 0;

    for (int bus = 0; bus < 8 && pos < 1900; bus++) {
        for (int dev = 0; dev < 32 && pos < 1900; dev++) {
            uint32_t id = 0;
            if (cosmo_pci_config_read(bus, dev, 0, 0, &id) < 0) continue;
            if (id == 0 || id == 0xFFFFFFFF) continue;

            uint32_t irq_reg = 0;
            cosmo_pci_config_read(bus, dev, 0, 0x3C, &irq_reg);
            int irq = (int)(irq_reg & 0xFF);

            /* bus_dev_fn (encoded: bus<<8 | dev<<3 | fn) */
            int bdf = (bus << 8) | (dev << 3);
            pos = append_hex(tmp, pos, 2048, (uint64_t)bdf);
            pos = append_str(tmp, pos, 2048, "\t");
            pos = append_hex(tmp, pos, 2048, (uint64_t)id);
            pos = append_str(tmp, pos, 2048, "\t");
            pos = append_int(tmp, pos, 2048, (long)irq);

            /* BAR addresses (registers 0x10..0x24) */
            for (int bar = 0; bar < 6; bar++) {
                uint32_t bval = 0;
                cosmo_pci_config_read(bus, dev, 0, 0x10 + bar * 4, &bval);
                pos = append_str(tmp, pos, 2048, "\t");
                pos = append_hex(tmp, pos, 2048, (uint64_t)bval);
            }
            pos = append_str(tmp, pos, 2048, "\n");
        }
    }

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/filesystems ─────────────────────────────── */

static int procfs_filesystems(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "\text4\n\tramfs\n\tprocfs\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

/* ── /proc/sys/kernel/tainted ──────────────────────── */

static int procfs_sys_tainted(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char tmp[] = "0\n";
    int pos = (int)sizeof(tmp) - 1;
    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/sys/kernel/pid_max ──────────────────────── */

static int procfs_sys_pid_max(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char tmp[16];
    int pos = itoa_buf(tmp, 16, (long)PID_MAX_CEILING);
    tmp[pos++] = '\n';

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/sys/fs/pipe-max-size ───────────────────── */

extern int  pipe_max_size_get(void);
extern long pipe_max_size_set(int v);

static int procfs_sys_pipe_max_size(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char tmp[16];
    int pos = itoa_buf(tmp, 16, (long)pipe_max_size_get());
    tmp[pos++] = '\n';

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* Parst fuehrenden Integer aus buf (ignoriert Whitespace + Newline danach).
 * Linux proc_dointvec: eine Zahl je write(), Trailing-Garbage erlaubt solange
 * Parser einen vollstaendigen Integer gelesen hat. */
static int procfs_parse_int(const char *buf, int size, long long *out) {
    int i = 0;
    while (i < size && (buf[i] == ' ' || buf[i] == '\t')) i++;
    int neg = 0;
    if (i < size && buf[i] == '-') { neg = 1; i++; }
    else if (i < size && buf[i] == '+') { i++; }
    int start = i;
    long long v = 0;
    while (i < size && buf[i] >= '0' && buf[i] <= '9') {
        long long d = buf[i] - '0';
        if (v > (0x7FFFFFFFFFFFFFFFLL - d) / 10) return -ERANGE;
        v = v * 10 + d;
        i++;
    }
    if (i == start) return -EINVAL;
    *out = neg ? -v : v;
    return i;
}

static long procfs_sys_pipe_max_size_write(const char *buf, int size,
                                           int offset, void *ctx) {
    (void)offset; (void)ctx;
    long long v;
    int consumed = procfs_parse_int(buf, size, &v);
    if (consumed < 0) return consumed;
    if (v < 0 || v > 0x7FFFFFFFLL) return -EINVAL;
    long r = pipe_max_size_set((int)v);
    if (r < 0) return r;
    return size;
}

/* ── /proc/sys/fs/lease-break-time ────────────────── */

static int procfs_sys_lease_break_time(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char tmp[8];
    int pos = itoa_buf(tmp, 8, 45L);
    tmp[pos++] = '\n';

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/sys/kernel/hostname ─────────────────────── */

static char hostname[64] = "cosmo";

static int procfs_sys_hostname(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char tmp[72];
    int pos = 0;
    pos = append_str(tmp, pos, 72, hostname);
    pos = append_str(tmp, pos, 72, "\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

/* ── /proc/pid/cwd ─────────────────────────────────── */

static int procfs_pid_cwd(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p || !p->cwd[0]) return 0;

    int len = 0;
    while (p->cwd[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = p->cwd[i];
    return out;
}

/* ── /proc/pid/environ ─────────────────────────────── */

static int procfs_pid_environ(char *buf, int size, int offset, void *ctx) {
    (void)ctx; (void)buf; (void)size; (void)offset;
    /* Environment not stored yet — return empty */
    return 0;
}

/* ── /proc/self/cmdline (or /proc/pid/cmdline) ───── */

static int procfs_pid_cmdline(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p || p->cmdline_len <= 0) return 0;

    int len = p->cmdline_len;
    if (offset >= len) return 0;
    int avail = len - offset;
    if (size > avail) size = avail;
    for (int i = 0; i < size; i++) buf[i] = p->cmdline[offset + i];
    return size;
}

/* ── /proc/pid/oom_score_adj ─ rw, range -1000..+1000 ────────────────────
 *
 * Linux fs/proc/base.c proc_oom_score_adj_{read,write}. Lowering past
 * oom_score_adj_min braucht CAP_SYS_RESOURCE (capabilities/setuid drop in
 * derselben Session koennten sonst die OOM-Immunitaet exfiltrieren).
 * Senken setzt das neue Minimum monoton. */

#include "linux/capability.h"

static int procfs_pid_oom_score_adj_read(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;
    char tmp[16];
    int n = itoa_buf(tmp, 16, (long)p->oom_score_adj);
    tmp[n++] = '\n';
    int out = 0;
    for (int i = offset; i < n && out < size; i++) buf[out++] = tmp[i];
    return out;
}

static long procfs_pid_oom_score_adj_write(const char *buf, int size,
                                           int offset, void *ctx) {
    (void)offset;
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return -EFAULT;

    long long v;
    int consumed = procfs_parse_int(buf, size, &v);
    if (consumed < 0) return consumed;
    if (v < OOM_SCORE_ADJ_MIN || v > OOM_SCORE_ADJ_MAX) return -EINVAL;

    /* Lowering past min braucht CAP_SYS_RESOURCE (Linux: __set_oom_adj). */
    process_t *caller = proc_current();
    int has_cap = caller && (caller->cap_effective & CAP_TO_MASK(CAP_SYS_RESOURCE));
    if (v < (long long)p->oom_score_adj_min && !has_cap) return -EPERM;

    p->oom_score_adj = (int16_t)v;
    if (v < (long long)p->oom_score_adj_min)
        p->oom_score_adj_min = (int16_t)v;
    return size;
}

/* ── /proc/pid/oom_score ─ ro, berechnet ─────────────────────────────── */

static int procfs_pid_oom_score_read(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;
    unsigned int total = (unsigned int)page_alloc_total();
    unsigned int score = oom_badness(p, total);
    char tmp[16];
    int n = itoa_buf(tmp, 16, (long)score);
    tmp[n++] = '\n';
    int out = 0;
    for (int i = offset; i < n && out < size; i++) buf[out++] = tmp[i];
    return out;
}

/* ── /proc/pid/oom_adj ─ legacy -17..+15, scaled ─────────────────────────
 *
 * Linux fs/proc/base.c (deprecated 2.6.36, removed 6.18+):
 *   oom_score_adj = oom_adj * OOM_SCORE_ADJ_MAX / -OOM_DISABLE
 *               = oom_adj * 1000 / 17 (oom_adj=-17 -> -1000, +15 -> +882)
 * Read: invers, gerundet zur naechsten ganzen Zahl.
 * Write: -17 → OOM_SCORE_ADJ_MIN special-case (sonst -1000 nicht
 *   erreichbar wegen Rundung). Sonst skalieren + clamp. */

static int procfs_pid_oom_adj_read(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;
    long adj_score = (long)p->oom_score_adj;
    long oom_adj;
    if (adj_score == OOM_SCORE_ADJ_MIN) oom_adj = OOM_DISABLE;
    else {
        /* Round half-to-zero (truncate). */
        oom_adj = (adj_score * (-OOM_DISABLE)) / OOM_SCORE_ADJ_MAX;
        if (oom_adj < OOM_ADJUST_MIN) oom_adj = OOM_ADJUST_MIN;
        if (oom_adj > OOM_ADJUST_MAX) oom_adj = OOM_ADJUST_MAX;
    }
    char tmp[16];
    int n = itoa_buf(tmp, 16, oom_adj);
    tmp[n++] = '\n';
    int out = 0;
    for (int i = offset; i < n && out < size; i++) buf[out++] = tmp[i];
    return out;
}

static long procfs_pid_oom_adj_write(const char *buf, int size,
                                     int offset, void *ctx) {
    (void)offset;
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return -EFAULT;

    long long v;
    int consumed = procfs_parse_int(buf, size, &v);
    if (consumed < 0) return consumed;
    if (v < OOM_ADJUST_MIN || v > OOM_ADJUST_MAX) return -EINVAL;

    long new_score;
    if (v == OOM_DISABLE) new_score = OOM_SCORE_ADJ_MIN;
    else                  new_score = (long)v * OOM_SCORE_ADJ_MAX / (-OOM_DISABLE);

    process_t *caller = proc_current();
    int has_cap = caller && (caller->cap_effective & CAP_TO_MASK(CAP_SYS_RESOURCE));
    if (new_score < (long)p->oom_score_adj_min && !has_cap) return -EPERM;

    p->oom_score_adj = (int16_t)new_score;
    if (new_score < (long)p->oom_score_adj_min)
        p->oom_score_adj_min = (int16_t)new_score;
    return size;
}

/* ── /proc/pid/maps (with process context) ────────── */

static int procfs_pid_maps(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;

    struct maps_ctx c = {
        .buf = buf, .max = size, .total = 0,
        .offset = offset, .written = 0,
        .stack_top = p->stack_top,
        .brk_base = p->brk_base,
        .brk_cur = p->brk_current,
    };
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    vma_walk_maps(p->vma_root, &c);
    spin_unlock_irq(&p->lock, irqf);
    return c.written;
}

/* ── Per-PID routing: /proc/<pid>/<file> ──────────── */

/* Parse leading decimal digits, return PID or -1 */
static int parse_pid(const char *s, const char **rest) {
    if (*s < '0' || *s > '9') return -1;
    int pid = 0;
    while (*s >= '0' && *s <= '9') {
        pid = pid * 10 + (*s - '0');
        s++;
    }
    if (*s != '/') return -1;
    *rest = s + 1;  /* skip '/' */
    return pid;
}

/* Try to handle /proc/<pid>/<file> or /proc/self/<file>.
 * name = everything after "/proc/" (e.g. "123/stat" or "self/cmdline").
 * Returns bytes read, or -1 if not a per-pid path. */
int procfs_pid_read(const char *name, char *buf, int size, int offset) {
    process_t *p = 0;
    const char *file = 0;

    /* "self/<file>" */
    if (name[0]=='s' && name[1]=='e' && name[2]=='l' && name[3]=='f' && name[4]=='/') {
        p = proc_current();
        file = name + 5;
    } else {
        /* "<pid>/<file>" */
        int pid = parse_pid(name, &file);
        if (pid < 0) return -1;
        p = proc_find((uint32_t)pid);
    }
    if (!p || !file) return -1;

    /* Dispatch to handler — compare file suffix */
    if (file[0]=='s' && file[1]=='t' && file[2]=='a' && file[3]=='t') {
        if (file[4]==0) return procfs_pid_stat(buf, size, offset, p);
        if (file[4]=='u' && file[5]=='s' && file[6]==0)
            return procfs_pid_status(buf, size, offset, p);  /* "status" */
    }
    if (file[0]=='m' && file[1]=='a' && file[2]=='p' && file[3]=='s' && file[4]==0)
        return procfs_pid_maps(buf, size, offset, p);
    if (file[0]=='c' && file[1]=='m' && file[2]=='d' && file[3]=='l' &&
        file[4]=='i' && file[5]=='n' && file[6]=='e' && file[7]==0)
        return procfs_pid_cmdline(buf, size, offset, p);
    if (file[0]=='c' && file[1]=='w' && file[2]=='d' && file[3]==0)
        return procfs_pid_cwd(buf, size, offset, p);
    if (file[0]=='e' && file[1]=='n' && file[2]=='v' && file[3]=='i' &&
        file[4]=='r' && file[5]=='o' && file[6]=='n' && file[7]==0)
        return procfs_pid_environ(buf, size, offset, p);
    if (file[0]=='o' && file[1]=='o' && file[2]=='m' && file[3]=='_') {
        if (file[4]=='s' && file[5]=='c' && file[6]=='o' && file[7]=='r' &&
            file[8]=='e') {
            if (file[9]==0)
                return procfs_pid_oom_score_read(buf, size, offset, p);
            if (file[9]=='_' && file[10]=='a' && file[11]=='d' &&
                file[12]=='j' && file[13]==0)
                return procfs_pid_oom_score_adj_read(buf, size, offset, p);
        }
        if (file[4]=='a' && file[5]=='d' && file[6]=='j' && file[7]==0)
            return procfs_pid_oom_adj_read(buf, size, offset, p);
    }

    return -1; /* not a per-pid file we handle */
}

/* Per-PID Write: derzeit nur OOM-Tuning-Pfade. Returns -EACCES fuer
 * read-only Pfade die zwar im Read-Dispatcher existieren, aber kein
 * Write-Pendant haben. */
long procfs_pid_write(const char *name, const char *buf, int size, int offset) {
    process_t *p = 0;
    const char *file = 0;

    if (name[0]=='s' && name[1]=='e' && name[2]=='l' && name[3]=='f' && name[4]=='/') {
        p = proc_current();
        file = name + 5;
    } else {
        int pid = parse_pid(name, &file);
        if (pid < 0) return -EBADF;
        p = proc_find((uint32_t)pid);
    }
    if (!p || !file) return -EBADF;

    if (file[0]=='o' && file[1]=='o' && file[2]=='m' && file[3]=='_') {
        if (file[4]=='s' && file[5]=='c' && file[6]=='o' && file[7]=='r' &&
            file[8]=='e' && file[9]=='_' && file[10]=='a' && file[11]=='d' &&
            file[12]=='j' && file[13]==0)
            return procfs_pid_oom_score_adj_write(buf, size, offset, p);
        if (file[4]=='a' && file[5]=='d' && file[6]=='j' && file[7]==0)
            return procfs_pid_oom_adj_write(buf, size, offset, p);
        if (file[4]=='s' && file[5]=='c' && file[6]=='o' && file[7]=='r' &&
            file[8]=='e' && file[9]==0)
            return -EACCES; /* oom_score is RO */
    }
    return -EACCES;
}

/* Check if a per-pid procfs path exists (for stat/open) */
int procfs_pid_exists(const char *name) {
    const char *file = 0;

    if (name[0]=='s' && name[1]=='e' && name[2]=='l' && name[3]=='f' && name[4]=='/') {
        file = name + 5;
    } else {
        int pid = parse_pid(name, &file);
        if (pid < 0) return 0;
        if (!proc_find((uint32_t)pid)) return 0;
    }
    if (!file) return 0;

    /* Known per-pid files */
    if (file[0]=='e' && file[1]=='x' && file[2]=='e' && file[3]==0) return 2; /* symlink */
    if (file[0]=='c' && file[1]=='m' && file[2]=='d' && file[3]=='l' &&
        file[4]=='i' && file[5]=='n' && file[6]=='e' && file[7]==0) return 1;
    if (file[0]=='s' && file[1]=='t' && file[2]=='a' && file[3]=='t') {
        if (file[4]==0) return 1;                           /* stat */
        if (file[4]=='u' && file[5]=='s' && file[6]==0) return 1; /* status */
    }
    if (file[0]=='m' && file[1]=='a' && file[2]=='p' && file[3]=='s' && file[4]==0) return 1;
    if (file[0]=='c' && file[1]=='w' && file[2]=='d' && file[3]==0) return 1;
    if (file[0]=='e' && file[1]=='n' && file[2]=='v' && file[3]=='i' &&
        file[4]=='r' && file[5]=='o' && file[6]=='n' && file[7]==0) return 1;
    if (file[0]=='f' && file[1]=='d' && file[2]==0) return 3; /* directory */
    /* OOM-Tuning: 4 = writable file (oom_score_adj, oom_adj),
     *             1 = read-only file (oom_score). */
    if (file[0]=='o' && file[1]=='o' && file[2]=='m' && file[3]=='_') {
        if (file[4]=='s' && file[5]=='c' && file[6]=='o' && file[7]=='r' &&
            file[8]=='e') {
            if (file[9]==0) return 1;
            if (file[9]=='_' && file[10]=='a' && file[11]=='d' &&
                file[12]=='j' && file[13]==0) return 4;
        }
        if (file[4]=='a' && file[5]=='d' && file[6]=='j' && file[7]==0)
            return 4;
    }

    /* /proc/<pid>/ns/time{,_for_children} — report as symlink-like entries
     * so stat() on the path succeeds. Actual open() bypasses procfs_open
     * and creates an FD_NSFS handle directly in do_openat. */
    if (file[0]=='n' && file[1]=='s' && file[2]=='/') {
        const char *sub = file + 3;
        if (sub[0]=='t' && sub[1]=='i' && sub[2]=='m' && sub[3]=='e') {
            if (sub[4] == 0) return 2;
            if (sub[4] == '_' && sub[5]=='f' && sub[6]=='o' && sub[7]=='r' &&
                sub[8] == '_' && sub[9]=='c' && sub[10]=='h' && sub[11]=='i' &&
                sub[12]=='l' && sub[13]=='d' && sub[14]=='r' && sub[15]=='e' &&
                sub[16]=='n' && sub[17] == 0)
                return 2;
        }
    }

    return 0;
}

/* ── /proc/self/timens_offsets — CLONE_NEWTIME offsets (write-only) ─────
 *
 * Linux: procfs entry created by proc_timens_set_offset on CONFIG_TIME_NS.
 * We surface the same API via procfs's write-handler hook. Writes target
 * the caller's time_ns_for_children — matches Linux timens_install. The
 * file is not readable (matches proc_tid_ns → open(.., O_RDONLY) returns
 * the raw offsets; we keep it write-only for now, tests don't read). */

static long procfs_timens_offsets_write(const char *buf, int size, int offset,
                                        void *ctx) {
    (void)offset; (void)ctx;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    struct time_namespace *ns = p->time_ns_for_children;
    if (!ns || ns == &init_time_ns) return -EACCES;
    if (ns->frozen) return -EACCES;

    /* Process buf line by line; Linux accepts multiple offsets per write,
     * one per line, and rolls back the whole write on any per-line error. */
    int total = 0;
    while (total < size) {
        /* Locate newline or end of buffer */
        int eol = total;
        while (eol < size && buf[eol] != '\n') eol++;
        int line_len = eol - total;
        /* Skip blank lines */
        int only_ws = 1;
        for (int i = total; i < eol; i++)
            if (buf[i] != ' ' && buf[i] != '\t') { only_ws = 0; break; }
        if (only_ws) {
            total = (eol < size) ? eol + 1 : eol;
            continue;
        }
        long r = time_ns_write_offset_line(ns, buf + total,
                                           line_len + (eol < size ? 1 : 0));
        if (r < 0) return r;
        total += (int)r;
    }
    return total;
}

/* ── Init ────────────────────────────────────────── */

__attribute__((cold))
void procfs_init(void) {
    num_entries = 0;
    procfs_register("dmesg", procfs_dmesg, 0);
    procfs_register("meminfo", procfs_meminfo, 0);
    procfs_register("cpuinfo", procfs_cpuinfo, 0);
    procfs_register("self/maps", procfs_pid_maps, 0);
    procfs_register("self/status", procfs_pid_status, 0);
    procfs_register("self/stat", procfs_pid_stat, 0);
    procfs_register("self/statm", procfs_self_statm, 0);
    procfs_register("self/cmdline", procfs_pid_cmdline, 0);
    procfs_register("sys/vm/overcommit_memory", procfs_overcommit, 0);
    procfs_register("self/cgroup", procfs_self_cgroup, 0);
    procfs_register("stat", procfs_global_stat, 0);
    procfs_register("uptime", procfs_uptime, 0);
    procfs_register("loadavg", procfs_loadavg, 0);
    procfs_register("version", procfs_version, 0);
    procfs_register("bus/pci/devices", procfs_pci_devices, 0);
    procfs_register("filesystems", procfs_filesystems, 0);
    procfs_register("sys/kernel/pid_max", procfs_sys_pid_max, 0);
    procfs_register("sys/kernel/tainted", procfs_sys_tainted, 0);
    procfs_register("sys/kernel/hostname", procfs_sys_hostname, 0);
    procfs_register_rw("sys/fs/pipe-max-size",
                       procfs_sys_pipe_max_size,
                       procfs_sys_pipe_max_size_write, 0);
    procfs_register("sys/fs/lease-break-time", procfs_sys_lease_break_time, 0);
    procfs_register("self/cwd", procfs_pid_cwd, 0);
    procfs_register("self/environ", procfs_pid_environ, 0);
    procfs_register_rw("self/timens_offsets", 0,
                       procfs_timens_offsets_write, 0);
    procfs_register("cmdline", procfs_cmdline, 0);
    procfs_register("mounts", procfs_mounts, 0);
    procfs_register("self/mounts", procfs_mounts, 0);
    procfs_register("sys/vm/mmap_min_addr", procfs_mmap_min_addr, 0);
    serial_puts("procfs: init (29 entries)\n");
}

/* ── VFS ops stubs (procfs uses its own dispatch via FD_PROCFS) ── */

#include "fs/vfs.h"

static int procfs_op_stat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    (void)mnt;
    const char *pn = relpath;
    if (pn[0] == '/') pn++;
    if (*pn == '\0') {
        /* /proc root directory */
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFDIR | 0555;
        buf->st_ino = 0xFFFF0000;
        buf->st_nlink = 2;
        buf->st_blksize = 4096;
        return 0;
    }
    int dummy;
    int pid_type = procfs_pid_exists(pn);
    if (procfs_stat(pn, &dummy) < 0 && !pid_type) return -ENOENT;
    kmemset(buf, 0, sizeof(struct k_stat));
    /* Writable entries advertise S_IWUSR for fopen("w"), O_WRONLY opens.
     * pid_type 4 = writable per-pid (oom_score_adj, oom_adj). */
    int writable = procfs_writable(pn) || pid_type == 4;
    uint32_t reg_mode = S_IFREG | S_IRUSR | (writable ? S_IWUSR : 0);
    buf->st_mode = (pid_type == 2) ? (S_IFLNK | 0777) :
                   (pid_type == 3) ? (S_IFDIR | 0555) :
                                     reg_mode;
    buf->st_ino = 0xFFFF0001;
    buf->st_blksize = 4096;
    return 0;
}

struct inode_ops procfs_inode_ops = {
    .stat = procfs_op_stat,
    .lstat = procfs_op_stat,
};

struct file_ops procfs_file_ops = {
    .read = 0, .write = 0, .lseek = 0, .pread = 0, .pwrite = 0,
    .close = 0, .fstat = 0, .ftruncate = 0, .fchmod = 0,
    .fchown = 0, .fsync = 0,
};
