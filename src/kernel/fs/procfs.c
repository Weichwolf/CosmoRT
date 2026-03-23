/* CosmoRT procfs — virtual filesystem at /proc
 *
 * Read-only. Entries registered with callbacks that generate content on read.
 * Max 32 entries, static allocation, no dynamic memory.
 */

#include "procfs.h"
#include "serial.h"
#include "page_alloc.h"
#include "smp.h"
#include "timer.h"

/* ── Entry table ─────────────────────────────────── */

#define PROCFS_MAX 32

typedef struct {
    char name[64];
    procfs_read_fn fn;
    void *ctx;
} procfs_entry_t;

static procfs_entry_t entries[PROCFS_MAX];
static int num_entries;

void procfs_register(const char *name, procfs_read_fn fn, void *ctx) {
    if (num_entries >= PROCFS_MAX) return;
    procfs_entry_t *e = &entries[num_entries++];
    int i = 0;
    while (name[i] && i < 63) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->fn = fn;
    e->ctx = ctx;
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
    return e->fn(buf, size, offset, e->ctx);
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

#include "vma.h"
#include "process.h"

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
};

static void vma_walk_maps(vma_t *node, struct maps_ctx *c) {
    if (!node) return;
    vma_walk_maps(node->left, c);

    /* Generate one line: <start>-<end> <perm> 00000000 00:00 0\n */
    char line[128];
    int lp = 0;
    lp = append_hex(line, lp, 128, node->start);
    line[lp++] = '-';
    lp = append_hex(line, lp, 128, node->end);
    line[lp++] = ' ';
    line[lp++] = (node->prot & PROT_READ)  ? 'r' : '-';
    line[lp++] = (node->prot & PROT_WRITE) ? 'w' : '-';
    line[lp++] = (node->prot & PROT_EXEC)  ? 'x' : '-';
    line[lp++] = 'p'; /* always private for now */
    line[lp++] = ' ';
    /* offset, dev, inode — all zero */
    const char *tail = "00000000 00:00 0\n";
    for (int i = 0; tail[i] && lp < 128; i++) line[lp++] = tail[i];

    /* Copy relevant portion into output buffer */
    for (int i = 0; i < lp; i++) {
        if (c->total >= c->offset && c->written < c->max)
            c->buf[c->written++] = line[i];
        c->total++;
    }

    vma_walk_maps(node->right, c);
}

static int procfs_self_maps(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    process_t *p = proc_current();
    if (!p) return 0;

    struct maps_ctx c = { buf, size, 0, offset, 0 };
    vma_walk_maps(p->vma_root, &c);
    return c.written;
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

/* ── Init ────────────────────────────────────────── */

void procfs_init(void) {
    num_entries = 0;
    procfs_register("dmesg", procfs_dmesg, 0);
    procfs_register("meminfo", procfs_meminfo, 0);
    procfs_register("cpuinfo", procfs_cpuinfo, 0);
    procfs_register("self/maps", procfs_self_maps, 0);
    serial_puts("procfs: init (4 entries)\n");
}
