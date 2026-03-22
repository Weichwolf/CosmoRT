/* Syscall subsystem internal header — shared between syscall .c files */
#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

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
int copy_path_from_user(char *kbuf, const char *upath, size_t max);

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

/* iovec for readv/writev */
struct iovec { const void *iov_base; size_t iov_len; };

/* ── Forward declarations: sys_file.c ── */
long do_read(int fd, void *buf, size_t count);
long do_write(int fd, const void *buf, size_t count);
long do_writev(int fd, const struct iovec *iov, int iovcnt);
long do_readv(int fd, const struct iovec *iov, int iovcnt);
long do_close(int fd);
long do_open(const char *path, int flags, int mode);
long do_openat(int dirfd, const char *path, int flags, int mode);
long do_lseek(int fd, long offset, int whence);
long do_fstat(int fd, struct k_stat *buf);
long do_stat(const char *path, struct k_stat *buf);
long do_lstat(const char *path, struct k_stat *buf);
long do_fstatat(int dirfd, const char *path, struct k_stat *buf, int flags);
long do_dup2(int oldfd, int newfd);
long do_dup3(int oldfd, int newfd, int flags);
long do_getcwd(char *buf, size_t size);
long do_chdir(const char *path);
long do_mkdir(const char *path, int mode);
long do_mkdirat(int dirfd, const char *path, int mode);
long do_rmdir(const char *path);
long do_unlink(const char *path);
long do_unlinkat(int dirfd, const char *path, int flags);
long do_rename(const char *oldpath, const char *newpath);
long do_renameat2(int olddirfd, const char *oldpath,
                  int newdirfd, const char *newpath, int flags);
long do_fchmod(int fd, uint32_t mode);
long do_fchown(int fd, uint32_t uid, uint32_t gid);
long do_link(const char *oldpath, const char *newpath);
long do_symlink(const char *target, const char *linkpath);
long do_readlink(const char *path, char *buf, size_t bufsiz);
long do_truncate(const char *path, int64_t length);
long do_ftruncate(int fd, int64_t length);
long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags);
long do_utimensat(int dirfd, const char *path, const void *utimes, int flags);
long do_fallocate(int fd, int mode, int64_t offset, int64_t len);
long do_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev);
long do_getdents64(int fd, void *buf, size_t count);
long do_ioctl(int fd, unsigned long request, unsigned long arg);
long do_fcntl(int fd, int cmd, long arg);

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

/* ── Forward declarations: sys_proc.c ── */
void do_exit(int status);
void do_exit_group(int status);
long do_clone(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls);
long do_uname(void *buf);
long do_getrandom(void *buf, size_t buflen, unsigned int flags);
long do_sched_setaffinity(int pid, size_t cpusetsize, const uint64_t *mask);
long do_sched_getaffinity(int pid, size_t cpusetsize, uint64_t *mask);
long do_sched_yield(void);
long do_sched_setscheduler(int pid, int policy, const void *param);
long do_sched_getscheduler(int pid);
long do_sched_setparam(int pid, const void *param);
long do_sched_getparam(int pid, void *param);
long do_arch_prctl(int code, unsigned long addr);
long do_sysinfo(void *info);
long do_getrusage(int who, void *usage);
long do_prlimit64(int pid, int resource, const void *new_rlim, void *old_rlim);
long do_times(void *buf);

/* ── Forward declarations: sys_signal.c ── */
void check_pending_signals(void);
void check_signals_syscall_path(long *result_ptr, long num);
long do_rt_sigaction(int sig, const void *act, void *oldact, size_t sigsetsize);
long do_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize);
long do_rt_sigreturn(void);
long do_kill(int pid, int sig);

/* k_timeval for gettimeofday */
struct k_timeval  { long tv_sec; long tv_usec; };

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
long pipe_close(fd_entry_t *fde);
long do_pipe2(int *fds, int flags);

/* Save user register state from syscall frame into thread_t */
void save_user_state_for_block(thread_t *t, long return_value);

#endif
