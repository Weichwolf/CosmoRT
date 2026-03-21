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

/* ── Built-in entries ────────────────────────────── */

static int procfs_dmesg(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    return serial_dmesg_read(buf, offset, size);
}

static int procfs_meminfo(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    int total = page_alloc_total();
    int free = page_alloc_free();
    int used = total - free;

    /* Generate full content, then apply offset */
    char tmp[256];
    int pos = 0;
    pos = append_str(tmp, pos, 256, "MemTotal: ");
    pos = append_int(tmp, pos, 256, (long)total * 4);
    pos = append_str(tmp, pos, 256, " kB\nMemFree:  ");
    pos = append_int(tmp, pos, 256, (long)free * 4);
    pos = append_str(tmp, pos, 256, " kB\nMemUsed:  ");
    pos = append_int(tmp, pos, 256, (long)used * 4);
    pos = append_str(tmp, pos, 256, " kB\n");

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

    char tmp[256];
    int pos = 0;
    pos = append_str(tmp, pos, 256, "cores:    ");
    pos = append_int(tmp, pos, 256, (long)cores);
    pos = append_str(tmp, pos, 256, "\ntsc_khz:  ");
    pos = append_int(tmp, pos, 256, (long)tsc_khz);
    pos = append_str(tmp, pos, 256, "\n");

    if (offset >= pos) return 0;
    int avail = pos - offset;
    if (size > avail) size = avail;
    for (int i = 0; i < size; i++) buf[i] = tmp[offset + i];
    return size;
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
    serial_puts("procfs: init (3 entries)\n");
}
