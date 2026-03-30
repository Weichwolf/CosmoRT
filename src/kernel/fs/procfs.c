/* CosmoRT procfs — virtual filesystem at /proc */

#include "fs/procfs.h"
#include "hw/serial.h"
#include "mm/page_alloc.h"
#include "core/smp.h"
#include "core/timer.h"

#define PROCFS_MAX 48

typedef int (*procfs_write_fn)(const char *buf, int len, void *ctx);

typedef struct {
    char name[64];
    procfs_read_fn fn;
    procfs_write_fn write_fn;
    void *ctx;
} procfs_entry_t;

static procfs_entry_t entries[PROCFS_MAX];
static int num_entries;

static void procfs_register_rw(const char *name, procfs_read_fn fn, procfs_write_fn wfn, void *ctx) {
    if (num_entries >= PROCFS_MAX) return;
    procfs_entry_t *e = &entries[num_entries++];
    int i = 0;
    while (name[i] && i < 63) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->fn = fn;
    e->write_fn = wfn;
    e->ctx = ctx;
}

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

int procfs_open(const char *name) {
    int idx = find_entry(name);
    if (idx < 0) return 0;
    return idx + 1;
}

int procfs_read(int handle, char *buf, int size, int offset) {
    if (handle < 1 || handle > num_entries) return 0;
    procfs_entry_t *e = &entries[handle - 1];
    return e->fn(buf, size, offset, e->ctx);
}

int procfs_write(int handle, const char *buf, int len) {
    if (handle < 1 || handle > num_entries) return -22;
    procfs_entry_t *e = &entries[handle - 1];
    if (!e->write_fn) return -13;
    return e->write_fn(buf, len, e->ctx);
}

void procfs_close(int handle) {
    (void)handle;
}

int procfs_stat(const char *name, int *size_out) {
    int idx = find_entry(name);
    if (idx < 0) return -1;
    if (size_out) *size_out = 0;
    return 0;
}

int procfs_iterate(int offset, int (*cb)(const char *name, void *ctx), void *ctx) {
    int visited = 0;
    for (int i = offset; i < num_entries; i++) {
        visited++;
        if (cb(entries[i].name, ctx)) break;
    }
    return visited;
}

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

static int append_int_rpad(char *buf, int pos, int max, long v, int width) {
    char tmp[20];
    int n = itoa_buf(tmp, 20, v);
    for (int i = 0; i < width - n && pos < max; i++) buf[pos++] = ' ';
    for (int i = 0; i < n && pos < max; i++) buf[pos++] = tmp[i];
    return pos;
}

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

#include "mm/vma.h"
#include "proc/process.h"

static int append_hex(char *buf, int pos, int max, uint64_t v) {
    char tmp[16];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
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

struct maps_ctx {
    char *buf;
    int max;
    int total;
    int offset;
    int written;
};

static void vma_walk_maps(vma_t *node, struct maps_ctx *c) {
    if (!node) return;
    vma_walk_maps(node->left, c);

    char line[128];
    int lp = 0;
    lp = append_hex(line, lp, 128, node->start);
    line[lp++] = '-';
    lp = append_hex(line, lp, 128, node->end);
    line[lp++] = ' ';
    line[lp++] = (node->prot & PROT_READ)  ? 'r' : '-';
    line[lp++] = (node->prot & PROT_WRITE) ? 'w' : '-';
    line[lp++] = (node->prot & PROT_EXEC)  ? 'x' : '-';
    line[lp++] = 'p';
    line[lp++] = ' ';
    const char *tail = "00000000 00:00 0\n";
    for (int i = 0; tail[i] && lp < 128; i++) line[lp++] = tail[i];

    for (int i = 0; i < lp; i++) {
        if (c->total >= c->offset && c->written < c->max)
            c->buf[c->written++] = line[i];
        c->total++;
    }

    vma_walk_maps(node->right, c);
}

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

static uint64_t vma_sum_size(vma_t *node) {
    if (!node) return 0;
    return vma_sum_size(node->left)
         + (node->end - node->start)
         + vma_sum_size(node->right);
}

static int procfs_pid_status(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;

    const char *state_str;
    switch (p->state) {
    case PROC_ALIVE:  state_str = "R (running)"; break;
    case PROC_ZOMBIE: state_str = "Z (zombie)";  break;
    default:          state_str = "S (sleeping)"; break;
    }

    uint64_t vm_bytes = 0;
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    vm_bytes = vma_sum_size(p->vma_root);
    spin_unlock_irq(&p->lock, irqf);
    long vm_kb = (long)(vm_bytes / 1024);

    char tmp[512];
    int pos = 0;
    pos = append_str(tmp, pos, 512, "Name:\t");
    pos = append_str(tmp, pos, 512, p->comm[0] ? p->comm : "?");
    pos = append_str(tmp, pos, 512, "\nState:\t");
    pos = append_str(tmp, pos, 512, state_str);
    pos = append_str(tmp, pos, 512, "\nPid:\t");
    pos = append_int(tmp, pos, 512, (long)p->pid);
    pos = append_str(tmp, pos, 512, "\nPPid:\t");
    pos = append_int(tmp, pos, 512, (long)p->parent_pid);
    pos = append_str(tmp, pos, 512, "\nThreads:\t");
    pos = append_int(tmp, pos, 512, (long)p->thread_count);
    pos = append_str(tmp, pos, 512, "\nVmSize:\t");
    pos = append_int(tmp, pos, 512, vm_kb);
    pos = append_str(tmp, pos, 512, " kB\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

static int procfs_pid_stat(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;

    char tmp[256];
    int pos = 0;
    pos = append_int(tmp, pos, 256, (long)p->pid);
    pos = append_str(tmp, pos, 256, " (");
    pos = append_str(tmp, pos, 256, p->comm[0] ? p->comm : "?");
    pos = append_str(tmp, pos, 256, ") R ");
    pos = append_int(tmp, pos, 256, (long)p->parent_pid);
    pos = append_str(tmp, pos, 256, " ");
    pos = append_int(tmp, pos, 256, (long)p->pgid);
    pos = append_str(tmp, pos, 256, " ");
    pos = append_int(tmp, pos, 256, (long)p->sid);
    pos = append_str(tmp, pos, 256, " 0 0 0 0 0 0 0 0 0 0 0 20 0 ");
    pos = append_int(tmp, pos, 256, (long)p->thread_count);
    pos = append_str(tmp, pos, 256, " 0 0 0 0");
    for (int i = 0; i < 20 && pos < 250; i++)
        pos = append_str(tmp, pos, 256, " 0");
    pos = append_str(tmp, pos, 256, "\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

static int procfs_overcommit(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    if (offset > 0) return 0;
    if (size < 2) return 0;
    buf[0] = '0'; buf[1] = '\n';
    return 2;
}

static int procfs_self_statm(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "0 0 0 0 0 0 0\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

static int oom_score_adj_value = 0;

static int procfs_oom_score_adj(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char s[16];
    int len = 0;
    int v = oom_score_adj_value;
    if (v < 0) { s[len++] = '-'; v = -v; }
    if (v >= 1000) { s[len++] = '0' + (v / 1000); v %= 1000; }
    if (v >= 100 || len > (oom_score_adj_value < 0 ? 1 : 0)) { s[len++] = '0' + (v / 100); v %= 100; }
    if (v >= 10 || len > (oom_score_adj_value < 0 ? 1 : 0)) { s[len++] = '0' + (v / 10); v %= 10; }
    s[len++] = '0' + v;
    s[len++] = '\n';
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

static int procfs_oom_score_adj_write(const char *buf, int len, void *ctx) {
    (void)ctx;
    int neg = 0, val = 0, i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\n')) i++;
    if (i < len && buf[i] == '-') { neg = 1; i++; }
    while (i < len && buf[i] >= '0' && buf[i] <= '9')
        val = val * 10 + (buf[i++] - '0');
    if (neg) val = -val;
    if (val < -1000) val = -1000;
    if (val > 1000) val = 1000;
    oom_score_adj_value = val;
    return len;
}

static int procfs_self_cgroup(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "0::/\n";
    int len = 5;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

static int procfs_global_stat(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    extern uint64_t timer_ms(void);
    uint64_t ms = timer_ms();
    long jiffies = (long)(ms / 10);

    char tmp[512];
    int pos = 0;
    pos = append_str(tmp, pos, 512, "cpu  0 0 0 ");
    pos = append_int(tmp, pos, 512, jiffies);
    pos = append_str(tmp, pos, 512, " 0 0 0 0 0 0\n");
    extern int smp_core_count(void);
    int ncores = smp_core_count();
    for (int c = 0; c < ncores && pos < 480; c++) {
        pos = append_str(tmp, pos, 512, "cpu");
        pos = append_int(tmp, pos, 512, (long)c);
        pos = append_str(tmp, pos, 512, " 0 0 0 ");
        pos = append_int(tmp, pos, 512, jiffies / ncores);
        pos = append_str(tmp, pos, 512, " 0 0 0 0 0 0\n");
    }
    pos = append_str(tmp, pos, 512, "ctxt 0\nbtime 0\nprocesses 1\n");

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

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

static int procfs_version(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "CosmoRT version 0.1.0 (gcc) #1 SMP\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

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

            int bdf = (bus << 8) | (dev << 3);
            pos = append_hex(tmp, pos, 2048, (uint64_t)bdf);
            pos = append_str(tmp, pos, 2048, "\t");
            pos = append_hex(tmp, pos, 2048, (uint64_t)id);
            pos = append_str(tmp, pos, 2048, "\t");
            pos = append_int(tmp, pos, 2048, (long)irq);

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

static int procfs_filesystems(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    const char *s = "\text2\n\tramfs\n\tprocfs\n";
    int len = 0; while (s[len]) len++;
    int out = 0;
    for (int i = offset; i < len && out < size; i++)
        buf[out++] = s[i];
    return out;
}

static int procfs_sys_pid_max(char *buf, int size, int offset, void *ctx) {
    (void)ctx;
    char tmp[16];
    int pos = itoa_buf(tmp, 16, (long)PID_TABLE_MAX);
    tmp[pos++] = '\n';

    int out = 0;
    for (int i = offset; i < pos && out < size; i++)
        buf[out++] = tmp[i];
    return out;
}

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

static int procfs_pid_environ(char *buf, int size, int offset, void *ctx) {
    (void)ctx; (void)buf; (void)size; (void)offset;
    return 0;
}

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

static int procfs_pid_maps(char *buf, int size, int offset, void *ctx) {
    process_t *p = ctx ? (process_t *)ctx : proc_current();
    if (!p) return 0;

    struct maps_ctx c = { buf, size, 0, offset, 0 };
    uint64_t irqf;
    spin_lock_irq(&p->lock, &irqf);
    vma_walk_maps(p->vma_root, &c);
    spin_unlock_irq(&p->lock, irqf);
    return c.written;
}

static int parse_pid(const char *s, const char **rest) {
    if (*s < '0' || *s > '9') return -1;
    int pid = 0;
    while (*s >= '0' && *s <= '9') {
        pid = pid * 10 + (*s - '0');
        s++;
    }
    if (*s != '/') return -1;
    *rest = s + 1;
    return pid;
}

int procfs_pid_read(const char *name, char *buf, int size, int offset) {
    process_t *p = 0;
    const char *file = 0;

    if (name[0]=='s' && name[1]=='e' && name[2]=='l' && name[3]=='f' && name[4]=='/') {
        p = proc_current();
        file = name + 5;
    } else {
        int pid = parse_pid(name, &file);
        if (pid < 0) return -1;
        p = proc_find((uint32_t)pid);
    }
    if (!p || !file) return -1;

    if (file[0]=='s' && file[1]=='t' && file[2]=='a' && file[3]=='t') {
        if (file[4]==0) return procfs_pid_stat(buf, size, offset, p);
        if (file[4]=='u' && file[5]=='s' && file[6]==0)
            return procfs_pid_status(buf, size, offset, p);
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
    {
        const char *oom = "oom_score_adj";
        int match = 1;
        for (int i = 0; oom[i]; i++) if (file[i] != oom[i]) { match = 0; break; }
        if (match) return procfs_oom_score_adj(buf, size, offset, p);
    }

    return -1;
}

int procfs_pid_write(const char *name, const char *buf, int len) {
    const char *file = 0;
    if (name[0]=='s' && name[1]=='e' && name[2]=='l' && name[3]=='f' && name[4]=='/')
        file = name + 5;
    else {
        int pid = parse_pid(name, &file);
        if (pid < 0) return -1;
    }
    if (!file) return -1;
    { const char *oom = "oom_score_adj"; int m = 1;
      for (int i = 0; oom[i]; i++) if (file[i] != oom[i]) { m = 0; break; }
      if (m) return procfs_oom_score_adj_write(buf, len, 0); }
    return -13;
}

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

    if (file[0]=='e' && file[1]=='x' && file[2]=='e' && file[3]==0) return 2;
    if (file[0]=='c' && file[1]=='m' && file[2]=='d' && file[3]=='l' &&
        file[4]=='i' && file[5]=='n' && file[6]=='e' && file[7]==0) return 1;
    if (file[0]=='s' && file[1]=='t' && file[2]=='a' && file[3]=='t') {
        if (file[4]==0) return 1;
        if (file[4]=='u' && file[5]=='s' && file[6]==0) return 1;
    }
    if (file[0]=='m' && file[1]=='a' && file[2]=='p' && file[3]=='s' && file[4]==0) return 1;
    if (file[0]=='c' && file[1]=='w' && file[2]=='d' && file[3]==0) return 1;
    if (file[0]=='e' && file[1]=='n' && file[2]=='v' && file[3]=='i' &&
        file[4]=='r' && file[5]=='o' && file[6]=='n' && file[7]==0) return 1;
    if (file[0]=='f' && file[1]=='d' && file[2]==0) return 3;
    { const char *oom = "oom_score_adj"; int m = 1;
      for (int i = 0; oom[i]; i++) if (file[i] != oom[i]) { m = 0; break; }
      if (m) return 1; }

    return 0;
}

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
    procfs_register("sys/kernel/hostname", procfs_sys_hostname, 0);
    procfs_register("self/cwd", procfs_pid_cwd, 0);
    procfs_register("self/environ", procfs_pid_environ, 0);
    procfs_register_rw("self/oom_score_adj", procfs_oom_score_adj, procfs_oom_score_adj_write, 0);
    serial_puts("procfs: init (22 entries)\n");
}
