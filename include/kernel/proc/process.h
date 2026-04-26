/* CosmoRT Process — address space + FD table + thread container */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "spinlock.h"
#include "proc/thread.h"
#include "event/fd.h"
#include "mm/vma.h"
#include "core/waitqueue.h"

/* Linux /proc/sys/kernel/ngroups_max default. The actual array is allocated
 * lazily via pages_alloc — process_t holds only a pointer + count, so a
 * process that never calls setgroups pays zero bytes for it. */
#define NGROUPS_MAX 65536

/* k_sigaction, SA_* flags — from linux.h (via syscall.h) */

/* Process states */
#define PROC_FREE    0
#define PROC_ALIVE   1
#define PROC_ZOMBIE  2

typedef struct process {
    /* ── Cache-line 0 (bytes 0-63): syscall hot path ──
     * state, pid, pml4 — checked on every syscall.
     * parent_pid, pgid, sid — getpid/setpgid/setsid. */
    int         state;
    uint32_t    pid;
    uint32_t    parent_pid;
    uint32_t    pgid;           /* Process Group ID */
    uint32_t    sid;            /* Session ID */
    int         exit_code;
    int         exit_signal;       /* 0 = normal exit, >0 = killed by signal */

    /* Job control state — consumed by wait4(WUNTRACED/WCONTINUED) */
    int         stop_signal;       /* signal that stopped the process (0 = not stopped) */
    int         was_continued;     /* 1 = continued since last wait4(WCONTINUED) */

    /* vfork: TID of parent thread blocked until this child execs/exits (0 = none) */
    int         vfork_parent_tid;

    /* CLONE_VM without CLONE_THREAD: child shares parent's pml4/vma_root.
     * Free paths must not touch these; exec/exit hands ownership back. */
    int         mm_shared;

    /* Address space */
    uint64_t   *pml4;

    /* ── Cache-line 1 (bytes 64+): memory management ── */
    /* Memory */
    uint64_t    brk_base;
    uint64_t    brk_current;
    uint64_t    brk_ceiling;   /* fast-reject: VMA collision boundary (0 = uncached) */
    uint64_t    mmap_next;

    /* VMA tree */
    vma_t      *vma_root;

    /* mlockall flags (MCL_CURRENT | MCL_FUTURE) */
    int         mlockall_flags;

    /* Driver capability: 1 = allowed to use HW primitive syscalls (512-520) */
    int         is_driver;

    /* Signals */
    uint64_t    sig_pending;        /* bitmask of pending signals */
    struct k_sigaction sig_actions[64]; /* per-signal action (0-63, SIGRTMIN=32..SIGRTMAX=63) */
    uint64_t    sig_trampoline_page; /* user-addr of RX trampoline page (0 = not yet allocated) */

    /* alarm(2): per-process SIGALRM timer (ms deadline, 0 = inactive) */
    uint64_t    alarm_deadline_ms;

    /* dnotify (F_NOTIFY): Ring pending {sig, fd}-Events fuer SA_SIGINFO.
     * Linux haelt rt-sigqueue pro (prozess, signal); wir serialisieren
     * die (sig, fd)-Paare in einer kleinen FIFO. Wird in deliver_signal
     * beim Passenden Signal geleert -> si_fd. */
    int         dnotify_q_sig[16];
    int         dnotify_q_fd[16];
    int         dnotify_q_head;
    int         dnotify_q_tail;

    /* Executable path (set by execve, read by /proc/self/exe) */
    char        exe_path[256];

    /* Executable identity for i_writecount write-deny tracking (ETXTBSY).
     * Set by execve(), cleared on proc_cleanup/execve-replace.
     * exe_backend: VFS_BACKEND_RAM or VFS_BACKEND_EXT4.
     * exe_ino: inode number inside that backend (0 = no deny held). */
    int         exe_backend;
    uint64_t    exe_ino;

    /* Command line (null-separated argv, set by execve, read by /proc/pid/cmdline) */
    char        cmdline[1024];
    int         cmdline_len;  /* total bytes including all nulls */

    /* Thread name (prctl PR_SET_NAME / PR_GET_NAME) */
    char        comm[16];

    /* Working directory */
    char        cwd[256];

    /* Chroot jail: "" = system root; all path lookups rewritten
     * "/x" -> "<root>/x". fork() inherits, execve() preserves. */
    char        root[256];

    /* File descriptors */
    fd_table_t  fds;

    /* Threads */
    thread_t   *main_thread;
    thread_t   *threads;      /* linked list */
    int         thread_count;

    spinlock_t  lock;

    /* Parent-side waitqueue for wait4: child state transitions
     * (exit/stop/continue) wake_up(&parent->children_wq). Linux's
     * task->signal->wait_chldexit equivalent. */
    wait_queue_head_t children_wq;

    /* Resource limits (at end to avoid offset shifts) */
    unsigned long rlim_nofile;   /* RLIMIT_NOFILE cur (0 = FD_DEFAULT_NOFILE) */
    unsigned long rlim_stack;    /* RLIMIT_STACK cur (0 = RLIM_STACK_DEFAULT) */
    unsigned long rlim_data;     /* RLIMIT_DATA cur (0 = unlimited, like Linux) */
    unsigned long rlim_nproc;    /* RLIMIT_NPROC cur (0 = unlimited) */
    unsigned long rlim_fsize;    /* RLIMIT_FSIZE cur (0 = unlimited) */
    unsigned long rlim_cpu_soft; /* RLIMIT_CPU cur seconds (0 = unlimited) */
    unsigned long rlim_cpu_hard; /* RLIMIT_CPU max seconds (0 = unlimited) */

    /* CPU accounting (updated by scheduler tick preemption).
     * cpu_time_ticks: accumulated ticks (1 tick = PERIODIC_TICKS_NS).
     * xcpu_last_sec: last second SIGXCPU was sent (for 1/sec rate on soft limit). */
    uint64_t    cpu_time_ticks;
    uint64_t    xcpu_last_sec;

    /* Signal to parent on exit (clone exit_signal, 0 = none → fallback SIGCHLD) */
    int         notify_signal;

    /* OOM-Killer-Tuning (Linux task->signal->oom_score_adj{,_min}).
     * oom_score_adj: -1000..+1000. -1000 = OOM-immun. +1000 = bevorzugtes Opfer.
     * oom_score_adj_min: monotone Untergrenze. Lowering past min braucht
     *   CAP_SYS_RESOURCE; einmal gesenkt bleibt es als neues Minimum stehen
     *   (Linux proc_oom_score_adj_write). Default 0/0; SUID-exec resetet 0/0. */
    int16_t     oom_score_adj;
    int16_t     oom_score_adj_min;

    /* Stack growth tracking */
    uint64_t    stack_top;       /* original stack top (set at exec) */

    /* File creation mask (umask) — default 0022 */
    uint32_t    umask_val;

    /* personality(2): Linux task->personality. Low byte = PER_*,
     * high bytes = Bug-Emulation-Flags (READ_IMPLIES_EXEC, ADDR_NO_RANDOMIZE…).
     * fork inherits, execve clears PER_CLEAR_ON_SETID on SUID-exec (future). */
    uint32_t    personality;

    /* POSIX Credentials — Linux struct cred subset.
     * Init: ruid=euid=suid=0, rgid=egid=sgid=0 (root).
     * fork: inherited. execve: inherited (SUID-exec handling future).
     * setuid/seteuid/setreuid/setresuid update per POSIX rules.
     * ngroups = number of valid supplementary GIDs in groups[]. */
    uint32_t    ruid, euid, suid, fsuid;
    uint32_t    rgid, egid, sgid, fsgid;
    /* Supplementary GIDs — Linux kernel.ngroups_max=65536. Lazy-allocated
     * via pages_alloc on the first setgroups(>0). NULL with ngroups==0 means
     * "never set". groups_pages tracks the pages_alloc order so cleanup can
     * call pages_free with the right count. */
    int         ngroups;
    int         groups_pages;
    uint32_t   *groups;

    /* POSIX Capabilities — Linux capget/capset ABI (V3: 64 bits in u32[2]).
     * Single-user kernel: proc_alloc() initialises everything granted.
     * fork() inherits, execve() preserves, capset() narrows per POSIX rules.
     * cap_bounding: per-task bounding set (Linux task->cred->cap_bset).
     *   Caps may only enter pI/pP from within bounding. Monotonic: prctl
     *   PR_CAPBSET_DROP narrows, never widens. fork inherits, execve keeps. */
    uint64_t    cap_effective;
    uint64_t    cap_permitted;
    uint64_t    cap_inheritable;
    uint64_t    cap_bounding;

    /* Time namespace (CLONE_NEWTIME). Linux task_struct::nsproxy::time_ns.
     * time_ns           — observed by clock_gettime et al.
     * time_ns_for_children — used by clone() for the new child.
     *
     * Both references hold refcounts. Init process starts with both →
     * &init_time_ns. On unshare(CLONE_NEWTIME) only time_ns_for_children
     * swaps to a fresh NS, matching Linux semantics (the current task
     * keeps its old time_ns). */
    struct time_namespace *time_ns;
    struct time_namespace *time_ns_for_children;

    /* Network namespace (CLONE_NEWNET). Linux task_struct::nsproxy::net_ns.
     * Inherited by fork without CLONE_NEWNET; CLONE_NEWNET allocates a
     * fresh NS for the child. unshare(CLONE_NEWNET) replaces this in-place;
     * setns(fd, CLONE_NEWNET) re-binds via /proc/<pid>/ns/net handle.
     * proc_alloc() initialises to &init_net_ns; proc_cleanup() drops the
     * refcount. */
    struct net_ns        *net_ns;
} process_t;

/* Supplementary group storage helpers. cred_groups_set is the single
 * mutator (replaces the array, frees pages on count==0). cred_groups_free
 * is called from proc_cleanup. Both are slab-free: they hand pages_alloc
 * pages back to the buddy allocator. */
int  cred_groups_set(struct process *p, const uint32_t *list, int count);
void cred_groups_free(struct process *p);

/* Credential helpers: in-group test (egid + supplementary) and
 * DAC access decision for a given inode-owner/mode.
 * cred_in_group: 1 if gid ∈ {egid} ∪ groups[], else 0.
 * cred_can_chmod: 1 if euid==0 or euid==owner_uid.
 * cred_can_chown_gid: non-root may only chown gid if euid==owner_uid and
 *   target_gid ∈ in-groups (egid or supplementary).
 * cred_must_clear_sgid: true if setting S_ISGID on a file-GID the caller
 *   is not a member of (used in chmod for non-root).
 * cred_owns: euid==owner_uid. */
int cred_in_group(process_t *p, uint32_t gid);
int cred_owns(process_t *p, uint32_t owner_uid);
int cred_can_chmod(process_t *p, uint32_t owner_uid);
int cred_can_chown_gid(process_t *p, uint32_t owner_uid,
                       uint32_t target_gid, int gid_specified);

/* DAC permission check on an inode's (uid, gid, mode) triple.
 * want is bitmask of MAY_READ|MAY_WRITE|MAY_EXEC.
 * Returns 0 on allow, -EACCES on deny. Root euid always passes. */
#define MAY_EXEC   1
#define MAY_WRITE  2
#define MAY_READ   4
int cred_may_access(process_t *p, uint32_t owner_uid, uint32_t owner_gid,
                    uint32_t mode, int want);

/* OOM-Killer ABI (Linux include/uapi/linux/oom.h, mm/oom_kill.c). */
#define OOM_SCORE_ADJ_MIN  (-1000)
#define OOM_SCORE_ADJ_MAX  ( 1000)
/* Legacy /proc/$pid/oom_adj (deprecated, removed in Linux >=6.18 — wir
 * unterstuetzen den Lese-/Schreib-Pfad noch fuer Tools wie oom_adj von oomd
 * und alter LTP-Suite). Range -17..+15 (OOM_DISABLE = -17). */
#define OOM_ADJUST_MIN     (-17)
#define OOM_ADJUST_MAX     ( 15)
#define OOM_DISABLE        (-17)

/* OOM-Killer-Entry (mm/oom.c). Versucht durch Killing eines Opfers Speicher
 * zurueckzugewinnen, weckt Aufrufer nach kurzem Delay zur Retry-Allokation.
 * Returns 1 wenn ein Opfer gewaehlt wurde (Retry sinnvoll), 0 wenn kein
 * Opfer existiert (echtes ENOMEM). Niemals init (PID 1) — panic stattdessen. */
int out_of_memory(void);

/* OOM-Score eines Tasks (Linux mm/oom_kill.c oom_badness):
 *   pages = rss + swap + pgtables
 *   points = pages * 1000 / total_pages + adj * pages_per_adjpoint
 *   immune wenn oom_score_adj == OOM_SCORE_ADJ_MIN.
 * Wir reportieren 0..2000 (Linux clamp), entspricht /proc/$pid/oom_score.
 * total_pages == 0 -> 0 zurueck. */
unsigned int oom_badness(struct process *p, unsigned int total_pages);

/* PID/TID ceiling — Linux kernel.pid_max default for 64-bit (2^22).
 * RLIMIT_NPROC caps per-user; this is only the absolute slot-index ceiling. */
#define PID_MAX_CEILING (1 << 22)

/* Current capacity of the pid_table / tid_table (dynamic, grows on demand).
 * Use these in iteration instead of a fixed compile-time constant. */
int pid_table_capacity(void);
int tid_table_capacity(void);

/* Iterate all live processes via pid_table. Callback returns 0 to continue, nonzero to stop. */
typedef int (*proc_iter_fn)(process_t *p, void *ctx);
void proc_for_each(proc_iter_fn fn, void *ctx);

/* Count live processes */
int proc_count_alive(void);

/* Initialize process subsystem (slab pools) */
void proc_init(void);

/* Create process from ELF binary. Returns PID or -1. */
int proc_create_elf(const void *elf_data, size_t elf_len);

/* Initialize a new thread's kstack with a context_switch frame that
 * "returns" into the userspace entry trampoline. Call after kstack alloc. */
void thread_init_kstack(thread_t *t);

/* Yield CPU: pick next thread, context_switch. Returns when rescheduled. */
void schedule(void);

/* Get current process (via percpu → current thread → process) */
process_t *proc_current(void);

/* Get current thread (via percpu) */
thread_t *thread_current(void);

/* Save user register state from syscall frame into thread_t for blocking */
void save_user_state_for_block(thread_t *t, long return_value);

/* Page table helpers */
uint64_t *alloc_page(void);
int map_user_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot);
int map_user_huge_page(uint64_t *user_pml4, uint64_t vaddr, uint64_t phys, int prot);

/* Process fork/exec */
long do_fork(unsigned long flags, void *child_stack,
             int *parent_tid, int *child_tid, unsigned long tls);
long do_vfork(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls);
long do_execve(const char *path, char *const argv[], char *const envp[]);
long do_execveat(int dirfd, const char *path, char *const argv[],
                 char *const envp[], int flags);
long do_wait4(int pid, int *wstatus, int options, void *rusage);

/* Process cleanup */
void proc_cleanup(process_t *p);
void free_address_space(uint64_t *pml4);

/* Find process by PID — O(1) via pid_table */
process_t *proc_find(uint32_t pid);

/* Find thread by TID — O(1) via tid_table */
thread_t *thread_find_by_tid(int tid);

#endif
