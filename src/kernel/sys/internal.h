/* Syscall subsystem internal header — shared between syscall .c files */
#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "sys/syscall.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "hw/serial.h"
#include "config.h"
#include "mm/vma.h"
#include "mm/page_alloc.h"
#include "ipc/futex.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "memops.h"
#include "uaccess.h"
#include "net/socket.h"
#include "net/unix_socket.h"
#include "net/udp.h"
#include "cosmort.h"
#include "net/net_port.h"
#include "core/irq.h"
#include "spinlock.h"
#include "fs/procfs.h"
#include "event/epoll.h"
#include "fs/ext2.h"
#include "vt/pty.h"
#include "vt/vt.h"
#include "arch/arch.h"

/* Copy user path string to kernel buffer with full bounds checking.
 * Returns string length (excluding NUL) or negative errno. */
#define PATH_MAX 4096
int copy_path_from_user(char *kbuf, const char *upath, size_t max);

/* Syscall saved frame layout (matches syscall_entry.asm push order) */
typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t rax;       /* syscall number */
    uint64_t r11;       /* user RFLAGS */
    uint64_t rcx;       /* user RIP */
} syscall_frame_t;

/* clone flags — from linux.h (via syscall.h) */

/* PTE flags */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PS       (1ULL << 7)   /* Page Size: 2MB huge page in PMD */
#define PTE_NX       (1ULL << 63)
#define PTE_COW      (1ULL << 9)
#define PTE_LAZYFREE (1ULL << 10)
#define PTE_DIRTY    (1ULL << 6)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define HUGE_PAGE_SIZE  (2ULL * 1024 * 1024)
#define HUGE_PAGE_MASK  (~(HUGE_PAGE_SIZE - 1))

/* iovec for readv/writev (POSIX: iov_base is void*, not const void*) */
struct iovec { void *iov_base; size_t iov_len; };

/* ── Shared helpers: sys_file.c + sys_fs.c ── */
int resolve_path(const char *path, char *out, int outsize);
int resolve_at_path(int dirfd, const char *upath, char *kpath, int max);

/* ── Forward declarations: sys_file.c ── */
long do_read(int fd, void *buf, size_t count);
long do_write(int fd, const void *buf, size_t count);
long do_writev(int fd, const struct iovec *iov, int iovcnt);
long do_readv(int fd, const struct iovec *iov, int iovcnt);
long do_close(int fd);
long do_open(const char *path, int flags, int mode);
long do_openat(int dirfd, const char *path, int flags, int mode);
long do_lseek(int fd, long offset, int whence);
long do_dup3(int oldfd, int newfd, int flags);
long do_getcwd(char *buf, size_t size);
long do_chdir(const char *path);
long do_getdents64(int fd, void *buf, size_t count);
long do_ioctl(int fd, unsigned long request, unsigned long arg);
long do_fcntl(int fd, int cmd, long arg);
long do_pread64(int fd, void *buf, size_t count, int64_t offset);
long do_pwrite64(int fd, const void *buf, size_t count, int64_t offset);
long do_fchdir(int fd);
long do_creat(const char *path, int mode);
long do_getdents(int fd, void *buf, size_t count);
long do_close_range(unsigned int first, unsigned int last, unsigned int flags);
long do_copy_file_range(int fd_in, long *off_in, int fd_out, long *off_out,
                        size_t len, unsigned int flags);
long do_memfd_create(const char *uname, unsigned int flags);

/* ── Forward declarations: sys_fs.c ── */
long do_fstat(int fd, struct k_stat *buf);
long do_fstatat(int dirfd, const char *path, struct k_stat *buf, int flags);
long do_mkdirat(int dirfd, const char *path, int mode);
long do_unlinkat(int dirfd, const char *path, int flags);
long do_renameat2(int olddirfd, const char *oldpath,
                  int newdirfd, const char *newpath, int flags);
long do_fchmod(int fd, uint32_t mode);
long do_fchown(int fd, uint32_t uid, uint32_t gid);
long do_linkat(int olddirfd, const char *oldpath,
               int newdirfd, const char *newpath, int flags);
long do_symlinkat(const char *target, int newdirfd, const char *linkpath);
long do_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
long do_truncate(const char *path, int64_t length);
long do_ftruncate(int fd, int64_t length);
long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags);
long do_utimensat(int dirfd, const char *path, const void *utimes, int flags);
long do_fallocate(int fd, int mode, int64_t offset, int64_t len);
long do_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev);
long do_faccessat(int dirfd, const char *path, int mode, int flags);
long do_statx(int dirfd, const char *pathname, int flags,
              unsigned int mask, void *statxbuf);
long do_statfs(const char *path, void *buf);
long do_fstatfs(int fd, void *buf);
long do_chown(const char *upath, uint32_t uid, uint32_t gid);
long do_fchownat(int dirfd, const char *upath, uint32_t uid, uint32_t gid, int flags);
long do_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
long do_faccessat2(int dirfd, const char *path, int mode, int flags);

/* ── Forward declarations: sys_mem.c ── */
long do_brk(unsigned long addr);
long do_mmap(unsigned long addr, size_t length, int prot,
             int flags, int fd, long offset);
long do_munmap(unsigned long addr, size_t length);
long do_mprotect(unsigned long addr, size_t len, int prot);
long do_mlock(unsigned long addr, size_t len);
long do_munlock(unsigned long addr, size_t len);
long do_mlockall(int flags);
long do_munlockall(void);
long do_madvise(unsigned long addr, size_t length, int advice);
long do_mremap(unsigned long old_addr, size_t old_size, size_t new_size,
               int flags, unsigned long new_addr);

/* ── Forward declarations: sys_proc.c ── */
void do_exit(int status);
void do_exit_group(int status);
long do_reboot(int magic1, int magic2, int cmd);
long do_clone(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls);
long do_uname(void *buf);
long do_getrandom(void *buf, size_t buflen, unsigned int flags);
long do_arch_prctl(int code, unsigned long addr);
long do_clone3(void *uargs, size_t size);
long do_sysinfo(void *info);
long do_getrusage(int who, void *usage);
long do_prlimit64(int pid, int resource, const void *new_rlim, void *old_rlim);
long do_times(void *buf);
long do_prctl(int option, unsigned long a2, unsigned long a3,
              unsigned long a4, unsigned long a5);
long do_getcpu(unsigned *cpu, unsigned *node);
long do_pause(void);
long do_getitimer(int which, void *curr_value);
long do_setitimer(int which, const void *new_value, void *old_value);
long do_waitid(int idtype, int id, void *infop, int options);

/* ── Forward declarations: sys_sched.c ── */
long do_sched_setaffinity(int pid, size_t cpusetsize, const uint64_t *mask);
long do_sched_getaffinity(int pid, size_t cpusetsize, uint64_t *mask);
long do_sched_yield(void);
long do_sched_setscheduler(int pid, int policy, const void *param);
long do_sched_getscheduler(int pid);
long do_sched_setparam(int pid, const void *param);
long do_sched_getparam(int pid, void *param);

/* ── Forward declarations: sys_signal.c ── */
void check_pending_signals(void);
long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize);
long do_kill(int pid, int sig);
long do_tgkill(int tgid, int tid, int sig);
long do_rt_sigsuspend(const uint64_t *mask, size_t sigsetsize);
long do_tkill(int tid, int sig);
long do_rt_sigpending(uint64_t *set, size_t sigsetsize);
long do_rt_sigtimedwait(const uint64_t *uset, void *uinfo, const struct k_timespec *uts, size_t sigsetsize);
long do_rt_sigqueueinfo(int pid, int sig, void *uinfo);

/* ── Forward declarations: sys_signal_frame.c ── */
void deliver_signal(thread_t *t, int signo);
long do_rt_sigreturn(void);

/* ── Forward declarations: sys_signal_handler.c ── */
void check_signals_syscall_path(long *result_ptr, long num);
long do_rt_sigaction(int sig, const void *act, void *oldact, size_t sigsetsize);
long do_sigaltstack(const void *ss, void *oss);
long do_alarm(unsigned int seconds);

/* Send SIGPIPE to current process. Returns -EPIPE always.
 * If handler is SIG_IGN, just returns -EPIPE without signal. */
long send_sigpipe(void);

/* k_timeval — from linux.h (via syscall.h) */

/* ── Forward declarations: sys_time.c ── */
long do_clock_gettime(int clk_id, struct k_timespec *tp);
long do_clock_getres(int clk_id, struct k_timespec *tp);
long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem);
long do_clock_nanosleep(int clk_id, int flags, const struct k_timespec *req, struct k_timespec *rem);
long do_gettimeofday(struct k_timeval *tv, void *tz);

/* ── Forward declarations: sys_ipc.c ── */
struct pipe;
struct pipe *pipe_from_fd(fd_entry_t *fde, int *is_write);
long pipe_read(struct pipe *pp, void *buf, size_t count);
long pipe_write(struct pipe *pp, const void *buf, size_t count);
long pipe_read_blocking(struct pipe *pp, void *buf, size_t count);
long pipe_write_blocking(struct pipe *pp, const void *buf, size_t count);
long pipe_close(fd_entry_t *fde);
long do_pipe2(int *fds, int flags);

/* ── Forward declarations: sys_net.c ── */
long do_sendmsg(int fd, const void *msg, int flags);
long do_recvmsg(int fd, void *msg, int flags);
long do_sendmmsg(int fd, uint64_t mmsg_arr, int vlen, int flags);
long do_recvmmsg(int fd, uint64_t mmsg_arr, int vlen, int flags);

/* ── Forward declarations: sys_event.c ── */
long do_pselect6(int nfds, uint64_t *readfds, long a3, long a4, long a5, long num);
long do_ppoll(long a1, long a2, long a3);

/* ── Forward declarations: cosmo_hw.c ── */
long do_cosmo_mmio_map(long a1, long a2, long a3);
long do_cosmo_dma_alloc(long a1, long a2, long a3);
long do_cosmo_dma_free(long a1, long a2);
long do_cosmo_irq_register(long a1, long a2, long a3);
long do_cosmo_pci_read(long a1, long a2, long a3, long a4, long a5);
long do_cosmo_pci_write(long a1, long a2, long a3, long a4, long a5);
long do_cosmo_fw_load(long a1, long a2, long a3);
long do_cosmo_nic_attach(long a1);
long do_cosmo_kexec(long a1, long a2);

/* ── Forward declarations: sys_id.c ── */
long do_getuid(void);
long do_getgid(void);
long do_geteuid(void);
long do_getegid(void);
long do_setuid(long uid);
long do_setgid(long gid);
long do_setreuid(long ruid, long euid);
long do_setregid(long rgid, long egid);
long do_setresuid(long ruid, long euid, long suid);
long do_setresgid(long rgid, long egid, long sgid);
long do_getresuid(long *ruid, long *euid, long *suid);
long do_getresgid(long *rgid, long *egid, long *sgid);
long do_setfsuid(long uid);
long do_setfsgid(long gid);

/* ── Forward declarations: stubs.c ── */
long do_set_robust_list(void *head, size_t len);
long do_get_robust_list(int pid, void **head_ptr, size_t *len_ptr);
long do_mount(void);
long do_sethostname(void);
long do_rseq(void);
long do_capget(void);
long do_capset(void);
long do_flock(int fd, int operation);
long do_msync(unsigned long addr, size_t length, int flags);
long do_sendfile(void);
long do_lchown(void);
long do_sched_get_priority_max(int policy);
long do_sched_get_priority_min(int policy);
long do_setrlimit(void);
long do_fadvise64(void);
long do_umask(int mask);
long do_getgroups(void);
long do_setgroups(void);
long do_personality(unsigned long persona);
long do_getpriority(int which, int who);
long do_setpriority(int which, int who, int prio);
long do_sync(void);
long do_syncfs(int fd);
long do_fsync(int fd);
long do_fdatasync(int fd);
long do_umount2(const char *target, int flags);
long do_utime(const char *filename, const void *times);
long do_mincore(void);
long do_ptrace(void);
long do_syslog_stub(void);
long do_sched_rr_get_interval(int pid, void *tp);
long do_vhangup(void);
long do_adjtimex(void);
long do_chroot(void);
long do_acct(void);
long do_settimeofday(void);
long do_setdomainname(void);
long do_readahead(void);
long do_restart_syscall(void);
long do_clock_settime(void);
long do_clock_adjtime(void);
long do_unshare_stub(void);
long do_utimes(const char *filename, const void *utimes_buf);
long do_futimesat(int dirfd, const char *filename, const void *utimes_buf);
long do_signalfd(int fd, const uint64_t *mask);
long do_eventfd(unsigned int initval);
long do_timerfd_gettime(int fd, void *curr_value);
long do_rt_tgsigqueueinfo(int tgid, int tid, int sig, void *uinfo);
long do_epoll_create(int size);
long do_inotify_init(void);
long do_preadv2(int fd, const void *iov, int iovcnt);
long do_pwritev2(int fd, const void *iov, int iovcnt);
long do_openat2(int dirfd, const char *pathname, void *how, size_t size);
long do_epoll_pwait2(int epfd, void *events, int maxevents, void *timeout);
long do_mknod(const char *path, uint32_t mode, uint64_t dev);
long do_fchmodat2(int dirfd, const char *path, uint32_t mode, int flags);

/* ── Forward declarations: sysv_ipc.c (SysV IPC) ── */
long do_msgget(int32_t key, int flags);
long do_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
long do_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);
long do_msgctl(int msqid, int cmd, void *buf);
long do_semget(int32_t key, int nsems, int flags);
long do_semop(int semid, const void *usops, size_t nsops);
long do_semctl(int semid, int semnum, int cmd, long arg4);
long do_shmget(int32_t key, size_t size, int flags);
long do_shmat(int shmid, const void *shmaddr, int shmflg);
long do_shmdt(const void *shmaddr);
long do_shmctl(int shmid, int cmd, void *buf);

/* Save user register state from syscall frame into thread_t */
void save_user_state_for_block(thread_t *t, long return_value);

#endif
