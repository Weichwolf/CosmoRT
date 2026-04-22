/* CosmoRT Time Namespace (CLONE_NEWTIME, Linux 5.6+)
 *
 * Per-process offset on CLOCK_MONOTONIC / CLOCK_BOOTTIME. Offsets are
 * applied on every read (clock_gettime) and on every absolute deadline
 * (clock_nanosleep TIMER_ABSTIME, timerfd_settime TFD_TIMER_ABSTIME).
 *
 * CLOCK_REALTIME is NOT namespace-able (Linux rule): wall time is global.
 *
 * Lifecycle:
 *   init_time_ns   — statically allocated, refcount pinned, frozen=1.
 *   time_ns_alloc  — fresh NS, refcount=1, frozen=0 (offsets writable).
 *   time_ns_get    — incref
 *   time_ns_put    — decref, free on last drop (never frees init_time_ns).
 *
 * Semantics (per Linux timens_offsets):
 *   - timens_offsets is writable ONLY while frozen==0. First syscall that
 *     reads/sleeps on an affected clock freezes the NS.
 *   - A process carries two references:
 *       task->time_ns              (affects clock_gettime etc.)
 *       task->time_ns_for_children (affects clone()/fork() children)
 *     unshare(CLONE_NEWTIME) sets *only* time_ns_for_children. The current
 *     task keeps its old time_ns — Linux: the task must fork() to observe
 *     the new offsets, or call execve() (kernel switches both references).
 *   - clone(CLONE_NEWTIME) is forbidden by Linux (returns -EINVAL) because
 *     a task can't create a new NS for itself atomically with fork.
 */
#ifndef COSMO_CORE_TIME_NS_H
#define COSMO_CORE_TIME_NS_H

#include <stdint.h>

#define TIME_NS_MAGIC 0x54494D4E53ULL /* 'TIMNS' */

struct time_namespace {
    uint64_t magic;          /* TIME_NS_MAGIC — detect use-after-free */
    int      refcount;       /* incref on dup/share, decref on free */
    int      frozen;         /* 1 = offsets locked (already read/used) */
    /* Offsets: added to raw clock values on read, subtracted from absolute
     * deadlines on sleep. Linux stores as struct timespec64. */
    int64_t  off_monotonic_sec;
    long     off_monotonic_nsec;
    int64_t  off_boottime_sec;
    long     off_boottime_nsec;
};

/* Boot namespace — shared, never freed. */
extern struct time_namespace init_time_ns;

void                    time_ns_init(void);
struct time_namespace  *time_ns_alloc(struct time_namespace *inherit);
struct time_namespace  *time_ns_get(struct time_namespace *ns);
void                    time_ns_put(struct time_namespace *ns);

/* Freeze the namespace (idempotent). Called on first affected clock use.
 * After freeze, writes to /proc/self/timens_offsets return -EACCES. */
void                    time_ns_freeze(struct time_namespace *ns);

/* Apply offset to a raw nanosecond count. Used by clock_gettime on
 * CLOCK_MONOTONIC*, CLOCK_BOOTTIME*. No-op on init_time_ns.
 * Returns raw_ns + offset_for_clock(ns, clk_id). */
int64_t                 time_ns_adjust_read(struct time_namespace *ns, int clk_id,
                                            int64_t raw_ns);

/* Inverse — subtract offset from an absolute user-provided deadline.
 * Used by clock_nanosleep(TIMER_ABSTIME) for CLOCK_MONOTONIC/BOOTTIME. */
int64_t                 time_ns_adjust_abs(struct time_namespace *ns, int clk_id,
                                           int64_t abs_ns);

/* Clock is subject to time_ns offsetting */
int                     time_ns_clock_affected(int clk_id);

/* ── NSFS handles ────────────────────────────────────────────
 *
 * open("/proc/self/ns/time") or ("/proc/self/ns/time_for_children") returns a
 * tiny heap object below. setns(fd, CLONE_NEWTIME) inspects this struct.
 *
 * kind == 0  → time (current task's time_ns at open-time)
 * kind == 1  → time_for_children (current task's time_ns_for_children at open-time)
 *
 * The NS pointer holds a reference (incref on open, decref on close).
 * The observable namespace may have moved on in the opening task since
 * then — this matches Linux: nsfs fd binds to a specific ns_common. */

struct nsfs_handle {
    int                      kind;
    struct time_namespace   *ns;
};

struct nsfs_handle *nsfs_handle_alloc(int kind, struct time_namespace *ns);
void                nsfs_handle_free(struct nsfs_handle *h);

/* Parse and apply one timens_offsets write line.
 *   Format: "<clockid> <sec> <nsec>\n" (decimal, possibly negative).
 *   Accepts CLOCK_MONOTONIC and CLOCK_BOOTTIME only (Linux restriction).
 * Returns number of bytes consumed on success, <0 on error:
 *   -EACCES: namespace already frozen.
 *   -EINVAL: bad clockid, bad nsec (>=1e9 or <0), or parse error.
 *   -ERANGE: sec would wrap.
 * Caller should iterate lines until the buffer is exhausted. */
long                    time_ns_write_offset_line(struct time_namespace *ns,
                                                  const char *line, int len);

#endif /* COSMO_CORE_TIME_NS_H */
