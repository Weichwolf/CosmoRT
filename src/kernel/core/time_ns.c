/* CosmoRT Time Namespace — CLONE_NEWTIME per-process clock offsets
 *
 * Layer: core (referenced by proc, sys/time, fs/procfs).
 */

#include "core/time_ns.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "linux/time.h"
#include "linux/errno.h"
#include "memops.h"
#include "hw/serial.h"
#include "hal/hal_cpu.h"

#define TIME_NS_INIT_SLAB_COUNT 4

static slab_t time_ns_slab;
static spinlock_t time_ns_lock = SPINLOCK_INIT;

struct time_namespace init_time_ns = {
    .magic              = TIME_NS_MAGIC,
    .refcount           = 0x40000000, /* pinned; never freed */
    .frozen             = 1,
    .off_monotonic_sec  = 0,
    .off_monotonic_nsec = 0,
    .off_boottime_sec   = 0,
    .off_boottime_nsec  = 0,
};

__attribute__((cold, noreturn))
static void time_ns_fail(const char *msg) {
    serial_puts("time_ns: FATAL ");
    serial_puts(msg);
    serial_puts("\n");
    for (;;) hal_cpu_halt_noirq();
}

__attribute__((cold))
void time_ns_init(void) {
    slab_init_dynamic(&time_ns_slab, (int)sizeof(struct time_namespace),
                      TIME_NS_INIT_SLAB_COUNT);
    serial_puts("time_ns: init\n");
}

struct time_namespace *time_ns_alloc(struct time_namespace *inherit) {
    struct time_namespace *ns = (struct time_namespace *)slab_alloc(&time_ns_slab);
    if (!ns) return 0;
    kmemset(ns, 0, sizeof(*ns));
    ns->magic    = TIME_NS_MAGIC;
    ns->refcount = 1;
    ns->frozen   = 0;
    if (inherit) {
        ns->off_monotonic_sec  = inherit->off_monotonic_sec;
        ns->off_monotonic_nsec = inherit->off_monotonic_nsec;
        ns->off_boottime_sec   = inherit->off_boottime_sec;
        ns->off_boottime_nsec  = inherit->off_boottime_nsec;
    }
    return ns;
}

struct time_namespace *time_ns_get(struct time_namespace *ns) {
    if (!ns) return 0;
    if (ns->magic != TIME_NS_MAGIC) time_ns_fail("time_ns_get: bad magic");
    uint64_t f;
    spin_lock_irq(&time_ns_lock, &f);
    ns->refcount++;
    spin_unlock_irq(&time_ns_lock, f);
    return ns;
}

void time_ns_put(struct time_namespace *ns) {
    if (!ns || ns == &init_time_ns) return;
    if (ns->magic != TIME_NS_MAGIC) time_ns_fail("time_ns_put: bad magic");
    uint64_t f;
    spin_lock_irq(&time_ns_lock, &f);
    int rc = --ns->refcount;
    spin_unlock_irq(&time_ns_lock, f);
    if (rc == 0) {
        ns->magic = 0; /* poison */
        slab_free(&time_ns_slab, ns);
    }
}

void time_ns_freeze(struct time_namespace *ns) {
    if (!ns || ns == &init_time_ns) return;
    /* Idempotent: single write, no barrier needed for x86 TSO given the
     * frozen-check reader is the same process for timens_offsets write. */
    ns->frozen = 1;
}

int time_ns_clock_affected(int clk_id) {
    return clk_id == CLOCK_MONOTONIC ||
           clk_id == CLOCK_MONOTONIC_RAW ||
           clk_id == CLOCK_MONOTONIC_COARSE ||
           clk_id == CLOCK_BOOTTIME;
}

/* Helper: fetch (sec, nsec) offset for a clock. Returns 1 if affected. */
static int get_offset(struct time_namespace *ns, int clk_id,
                      int64_t *sec, long *nsec) {
    if (!ns || ns == &init_time_ns) return 0;
    switch (clk_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
        *sec  = ns->off_monotonic_sec;
        *nsec = ns->off_monotonic_nsec;
        return 1;
    case CLOCK_BOOTTIME:
        *sec  = ns->off_boottime_sec;
        *nsec = ns->off_boottime_nsec;
        return 1;
    default:
        return 0;
    }
}

int64_t time_ns_adjust_read(struct time_namespace *ns, int clk_id, int64_t raw_ns) {
    int64_t sec = 0; long nsec = 0;
    if (!get_offset(ns, clk_id, &sec, &nsec)) return raw_ns;
    time_ns_freeze(ns);
    return raw_ns + sec * NSEC_PER_SEC + (int64_t)nsec;
}

int64_t time_ns_adjust_abs(struct time_namespace *ns, int clk_id, int64_t abs_ns) {
    int64_t sec = 0; long nsec = 0;
    if (!get_offset(ns, clk_id, &sec, &nsec)) return abs_ns;
    time_ns_freeze(ns);
    return abs_ns - sec * NSEC_PER_SEC - (int64_t)nsec;
}

/* ── timens_offsets write parser ─────────────────────────────
 *
 * Linux grammar (one line per offset):
 *   <clockid> <sec> <nsec>\n
 *
 * clockid: decimal int, must be CLOCK_MONOTONIC or CLOCK_BOOTTIME.
 * sec    : signed decimal, absolute value < 2^32 (Linux clamps earlier).
 * nsec   : unsigned decimal < 1e9. Negative line is expressed as sec<0.
 *
 * On a frozen namespace every line returns -EACCES. On per-line parse
 * error the partial state is still committed — matches Linux behaviour,
 * which writes offsets independently inside proc_timens_set_offset.
 */

static int parse_int(const char *s, int len, int *pos, long long *out, int allow_neg) {
    int i = *pos;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    int neg = 0;
    if (allow_neg && i < len && s[i] == '-') { neg = 1; i++; }
    else if (i < len && s[i] == '+')          { i++; }
    int start = i;
    long long v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        long long d = s[i] - '0';
        if (v > (0x7FFFFFFFFFFFFFFFLL - d) / 10) return -ERANGE;
        v = v * 10 + d;
        i++;
    }
    if (i == start) return -EINVAL;
    *out = neg ? -v : v;
    *pos = i;
    return 0;
}

long time_ns_write_offset_line(struct time_namespace *ns, const char *line, int len) {
    if (!ns) return -EINVAL;
    if (ns == &init_time_ns) return -EACCES;
    if (ns->frozen) return -EACCES;
    if (len <= 0) return -EINVAL;

    int pos = 0;
    long long clk_id = 0, sec = 0, nsec = 0;
    int r;

    r = parse_int(line, len, &pos, &clk_id, 1);
    if (r < 0) return r;
    r = parse_int(line, len, &pos, &sec, 1);
    if (r < 0) return r;
    r = parse_int(line, len, &pos, &nsec, 1);
    if (r < 0) return r;

    /* Optional trailing whitespace + newline */
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    int consumed;
    if (pos < len && line[pos] == '\n') { consumed = pos + 1; }
    else if (pos == len)                { consumed = pos; }
    else                                { return -EINVAL; }

    if (nsec < 0 || nsec >= NSEC_PER_SEC) return -EINVAL;
    /* Linux limits offset magnitude to "ktime_t-safe". Keep generous bound. */
    if (sec < -0x7FFFFFFFLL || sec > 0x7FFFFFFFLL) return -ERANGE;

    switch ((int)clk_id) {
    case CLOCK_MONOTONIC:
        ns->off_monotonic_sec  = (int64_t)sec;
        ns->off_monotonic_nsec = (long)nsec;
        break;
    case CLOCK_BOOTTIME:
        ns->off_boottime_sec  = (int64_t)sec;
        ns->off_boottime_nsec = (long)nsec;
        break;
    default:
        return -EINVAL;
    }
    return consumed;
}
