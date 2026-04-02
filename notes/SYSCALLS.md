# CosmoRT Syscall-Status (x86_64)

Stand: 2026-03-25

Legende:
- **IMPL** = Echte Logik implementiert
- **STUB** = Definiert, aber return 0 / -ENOSYS / -ENODATA / Minimalverhalten
- **FEHLT** = Nicht implementiert (kein case in dispatch, default → -ENOSYS)

| Nr | Name | Status | Datei |
|----|------|--------|-------|
| 0 | read | IMPL | src/kernel/syscall/sys_file.c |
| 1 | write | IMPL | src/kernel/syscall/sys_file.c |
| 2 | open | IMPL | src/kernel/syscall/sys_file.c |
| 3 | close | IMPL | src/kernel/syscall/sys_file.c |
| 4 | stat | IMPL | src/kernel/syscall/sys_file.c |
| 5 | fstat | IMPL | src/kernel/syscall/sys_file.c |
| 6 | lstat | IMPL | src/kernel/syscall/sys_file.c |
| 7 | poll | IMPL | src/kernel/syscall/dispatch.c |
| 8 | lseek | IMPL | src/kernel/syscall/sys_file.c |
| 9 | mmap | IMPL | src/kernel/syscall/sys_mem.c |
| 10 | mprotect | IMPL | src/kernel/syscall/sys_mem.c |
| 11 | munmap | IMPL | src/kernel/syscall/sys_mem.c |
| 12 | brk | IMPL | src/kernel/syscall/sys_mem.c |
| 13 | rt_sigaction | IMPL | src/kernel/syscall/sys_signal.c |
| 14 | rt_sigprocmask | IMPL | src/kernel/syscall/sys_signal.c |
| 15 | rt_sigreturn | IMPL | src/kernel/syscall/sys_signal.c |
| 16 | ioctl | IMPL | src/kernel/syscall/sys_file.c |
| 17 | pread64 | IMPL | src/kernel/syscall/sys_file.c |
| 18 | pwrite64 | IMPL | src/kernel/syscall/sys_file.c |
| 19 | readv | IMPL | src/kernel/syscall/sys_file.c |
| 20 | writev | IMPL | src/kernel/syscall/sys_file.c |
| 21 | access | IMPL | src/kernel/syscall/sys_file.c |
| 22 | pipe | IMPL | src/kernel/syscall/sys_ipc.c |
| 23 | select | IMPL | src/kernel/syscall/dispatch.c |
| 24 | sched_yield | IMPL | src/kernel/syscall/sys_proc.c |
| 25 | mremap | IMPL | src/kernel/syscall/sys_mem.c |
| 26 | msync | STUB | syscall_table.h (return 0) |
| 27 | mincore | FEHLT | — |
| 28 | madvise | IMPL | src/kernel/syscall/sys_mem.c |
| 29 | shmget | FEHLT | — |
| 30 | shmat | FEHLT | — |
| 31 | shmctl | FEHLT | — |
| 32 | dup | IMPL | src/kernel/syscall/dispatch.c |
| 33 | dup2 | IMPL | src/kernel/syscall/dispatch.c |
| 34 | pause | FEHLT | — |
| 35 | nanosleep | IMPL | src/kernel/syscall/sys_time.c |
| 36 | getitimer | FEHLT | — |
| 37 | alarm | FEHLT | — |
| 38 | setitimer | FEHLT | — |
| 39 | getpid | IMPL | src/kernel/syscall/dispatch.c |
| 40 | sendfile | STUB | syscall_table.h (return -ENOSYS) |
| 41 | socket | IMPL | src/kernel/net/socket.c |
| 42 | connect | IMPL | src/kernel/net/socket.c |
| 43 | accept | IMPL | src/kernel/net/socket.c |
| 44 | sendto | IMPL | src/kernel/net/socket.c |
| 45 | recvfrom | IMPL | src/kernel/net/socket.c |
| 46 | sendmsg | IMPL | src/kernel/syscall/dispatch.c |
| 47 | recvmsg | IMPL | src/kernel/syscall/dispatch.c |
| 48 | shutdown | IMPL | src/kernel/net/socket.c |
| 49 | bind | IMPL | src/kernel/net/socket.c |
| 50 | listen | IMPL | src/kernel/net/socket.c |
| 51 | getsockname | IMPL | src/kernel/net/socket.c |
| 52 | getpeername | IMPL | src/kernel/net/socket.c |
| 53 | socketpair | IMPL | src/kernel/syscall/dispatch.c |
| 54 | setsockopt | IMPL | src/kernel/net/socket.c |
| 55 | getsockopt | IMPL | src/kernel/net/socket.c |
| 56 | clone | IMPL | src/kernel/syscall/sys_proc.c |
| 57 | fork | IMPL | src/kernel/proc/process.c |
| 58 | vfork | IMPL | src/kernel/proc/process.c |
| 59 | execve | IMPL | src/kernel/proc/process.c |
| 60 | exit | IMPL | src/kernel/syscall/sys_proc.c |
| 61 | wait4 | IMPL | src/kernel/proc/process.c |
| 62 | kill | IMPL | src/kernel/syscall/sys_signal.c |
| 63 | uname | IMPL | src/kernel/syscall/sys_proc.c |
| 64 | semget | FEHLT | — |
| 65 | semop | FEHLT | — |
| 66 | semctl | FEHLT | — |
| 67 | shmdt | FEHLT | — |
| 68 | msgget | FEHLT | — |
| 69 | msgsnd | FEHLT | — |
| 70 | msgrcv | FEHLT | — |
| 71 | msgctl | FEHLT | — |
| 72 | fcntl | IMPL | src/kernel/syscall/sys_file.c |
| 73 | flock | FEHLT | — |
| 74 | fsync | FEHLT | — |
| 75 | fdatasync | FEHLT | — |
| 76 | truncate | IMPL | src/kernel/syscall/sys_file.c |
| 77 | ftruncate | IMPL | src/kernel/syscall/sys_file.c |
| 78 | getdents | FEHLT | — |
| 79 | getcwd | IMPL | src/kernel/syscall/sys_file.c |
| 80 | chdir | IMPL | src/kernel/syscall/sys_file.c |
| 81 | fchdir | FEHLT | — |
| 82 | rename | IMPL | src/kernel/syscall/sys_file.c |
| 83 | mkdir | IMPL | src/kernel/syscall/sys_file.c |
| 84 | rmdir | IMPL | src/kernel/syscall/sys_file.c |
| 85 | creat | FEHLT | — |
| 86 | link | IMPL | src/kernel/syscall/sys_file.c |
| 87 | unlink | IMPL | src/kernel/syscall/sys_file.c |
| 88 | symlink | IMPL | src/kernel/syscall/sys_file.c |
| 89 | readlink | IMPL | src/kernel/syscall/sys_file.c |
| 90 | chmod | IMPL | src/kernel/syscall/sys_file.c |
| 91 | fchmod | IMPL | src/kernel/syscall/sys_file.c |
| 92 | chown | FEHLT | — |
| 93 | fchown | IMPL | src/kernel/syscall/sys_file.c |
| 94 | lchown | STUB | syscall_table.h (return 0) |
| 95 | umask | STUB | syscall_table.h (return 0022) |
| 96 | gettimeofday | IMPL | src/kernel/syscall/sys_time.c |
| 97 | getrlimit | IMPL | src/kernel/syscall/sys_proc.c |
| 98 | getrusage | IMPL | src/kernel/syscall/sys_proc.c |
| 99 | sysinfo | IMPL | src/kernel/syscall/sys_proc.c |
| 100 | times | IMPL | src/kernel/syscall/sys_proc.c |
| 101 | ptrace | FEHLT | — |
| 102 | getuid | STUB | syscall_table.h (return 0) |
| 103 | syslog | FEHLT | — |
| 104 | getgid | STUB | syscall_table.h (return 0) |
| 105 | setuid | FEHLT | — |
| 106 | setgid | FEHLT | — |
| 107 | geteuid | STUB | syscall_table.h (return 0) |
| 108 | getegid | STUB | syscall_table.h (return 0) |
| 109 | setpgid | IMPL | src/kernel/syscall/dispatch.c |
| 110 | getppid | IMPL | src/kernel/syscall/dispatch.c |
| 111 | getpgrp | IMPL | src/kernel/syscall/dispatch.c |
| 112 | setsid | IMPL | src/kernel/syscall/dispatch.c |
| 113 | setreuid | FEHLT | — |
| 114 | setregid | FEHLT | — |
| 115 | getgroups | STUB | syscall_table.h (return 0) |
| 116 | setgroups | STUB | syscall_table.h (return 0) |
| 117 | setresuid | FEHLT | — |
| 118 | getresuid | FEHLT | — |
| 119 | setresgid | FEHLT | — |
| 120 | getresgid | FEHLT | — |
| 121 | getpgid | IMPL | src/kernel/syscall/dispatch.c |
| 122 | setfsuid | FEHLT | — |
| 123 | setfsgid | FEHLT | — |
| 124 | getsid | IMPL | src/kernel/syscall/dispatch.c |
| 125 | capget | STUB | syscall_table.h (return -EPERM) |
| 126 | capset | STUB | syscall_table.h (return -EPERM) |
| 127 | rt_sigpending | FEHLT | — |
| 128 | rt_sigtimedwait | FEHLT | — |
| 129 | rt_sigqueueinfo | FEHLT | — |
| 130 | rt_sigsuspend | IMPL | src/kernel/syscall/sys_signal.c |
| 131 | sigaltstack | IMPL | src/kernel/syscall/sys_signal.c |
| 132 | utime | FEHLT | — |
| 133 | mknod | FEHLT | — |
| 134 | uselib | FEHLT | — |
| 135 | personality | FEHLT | — |
| 136 | ustat | FEHLT | — |
| 137 | statfs | IMPL | src/kernel/syscall/sys_proc.c |
| 138 | fstatfs | IMPL | src/kernel/syscall/sys_proc.c |
| 139 | sysfs | FEHLT | — |
| 140 | getpriority | FEHLT | — |
| 141 | setpriority | FEHLT | — |
| 142 | sched_setparam | IMPL | src/kernel/syscall/sys_proc.c |
| 143 | sched_getparam | IMPL | src/kernel/syscall/sys_proc.c |
| 144 | sched_setscheduler | IMPL | src/kernel/syscall/sys_proc.c |
| 145 | sched_getscheduler | IMPL | src/kernel/syscall/sys_proc.c |
| 146 | sched_get_priority_max | STUB | syscall_table.h (return 31) |
| 147 | sched_get_priority_min | STUB | syscall_table.h (return 0) |
| 148 | sched_rr_get_interval | FEHLT | — |
| 149 | mlock | IMPL | src/kernel/syscall/sys_mem.c |
| 150 | munlock | IMPL | src/kernel/syscall/sys_mem.c |
| 151 | mlockall | IMPL | src/kernel/syscall/sys_mem.c |
| 152 | munlockall | IMPL | src/kernel/syscall/sys_mem.c |
| 153 | vhangup | FEHLT | — |
| 154 | modify_ldt | FEHLT | — |
| 155 | pivot_root | FEHLT | — |
| 156 | _sysctl | FEHLT | — |
| 157 | prctl | IMPL | src/kernel/syscall/sys_proc.c |
| 158 | arch_prctl | IMPL | src/kernel/syscall/sys_proc.c |
| 159 | adjtimex | FEHLT | — |
| 160 | setrlimit | STUB | syscall_table.h (return 0) |
| 161 | chroot | FEHLT | — |
| 162 | sync | FEHLT | — |
| 163 | acct | FEHLT | — |
| 164 | settimeofday | FEHLT | — |
| 165 | mount | STUB | syscall_table.h (return 0) |
| 166 | umount2 | FEHLT | — |
| 167 | swapon | FEHLT | — |
| 168 | swapoff | FEHLT | — |
| 169 | reboot | FEHLT | — |
| 170 | sethostname | STUB | syscall_table.h (return 0) |
| 171 | setdomainname | FEHLT | — |
| 172 | iopl | FEHLT | — |
| 173 | ioperm | FEHLT | — |
| 174 | create_module | FEHLT | — |
| 175 | init_module | FEHLT | — |
| 176 | delete_module | FEHLT | — |
| 177 | get_kernel_syms | FEHLT | — |
| 178 | query_module | FEHLT | — |
| 179 | quotactl | FEHLT | — |
| 180 | nfsservctl | FEHLT | — |
| 181 | getpmsg | FEHLT | — |
| 182 | putpmsg | FEHLT | — |
| 183 | afs_syscall | FEHLT | — |
| 184 | tuxcall | FEHLT | — |
| 185 | security | FEHLT | — |
| 186 | gettid | IMPL | src/kernel/syscall/dispatch.c |
| 187 | readahead | FEHLT | — |
| 188 | setxattr | STUB | dispatch.c (return -ENODATA) |
| 189 | lsetxattr | STUB | dispatch.c (return -ENODATA) |
| 190 | fsetxattr | STUB | dispatch.c (return -ENODATA) |
| 191 | getxattr | STUB | dispatch.c (return -ENODATA) |
| 192 | lgetxattr | STUB | dispatch.c (return -ENODATA) |
| 193 | fgetxattr | STUB | dispatch.c (return -ENODATA) |
| 194 | listxattr | STUB | dispatch.c (return -ENODATA) |
| 195 | llistxattr | STUB | dispatch.c (return -ENODATA) |
| 196 | flistxattr | STUB | dispatch.c (return -ENODATA) |
| 197 | removexattr | STUB | dispatch.c (return -ENODATA) |
| 198 | lremovexattr | STUB | dispatch.c (return -ENODATA) |
| 199 | fremovexattr | STUB | dispatch.c (return -ENODATA) |
| 200 | tkill | FEHLT | — |
| 201 | time | IMPL | src/kernel/syscall/dispatch.c |
| 202 | futex | IMPL | src/kernel/ipc/futex.c |
| 203 | sched_setaffinity | IMPL | src/kernel/syscall/sys_proc.c |
| 204 | sched_getaffinity | IMPL | src/kernel/syscall/sys_proc.c |
| 205 | set_thread_area | FEHLT | — |
| 206 | io_setup | FEHLT | — |
| 207 | io_destroy | FEHLT | — |
| 208 | io_getevents | FEHLT | — |
| 209 | io_submit | FEHLT | — |
| 210 | io_cancel | FEHLT | — |
| 211 | get_thread_area | FEHLT | — |
| 212 | lookup_dcookie | FEHLT | — |
| 213 | epoll_create | FEHLT | — |
| 214 | epoll_ctl_old | FEHLT | — |
| 215 | epoll_wait_old | FEHLT | — |
| 216 | remap_file_pages | FEHLT | — |
| 217 | getdents64 | IMPL | src/kernel/syscall/sys_file.c |
| 218 | set_tid_address | IMPL | src/kernel/syscall/dispatch.c |
| 219 | restart_syscall | FEHLT | — |
| 220 | semtimedop | FEHLT | — |
| 221 | fadvise64 | STUB | syscall_table.h (return 0) |
| 222 | timer_create | FEHLT | — |
| 223 | timer_settime | FEHLT | — |
| 224 | timer_gettime | FEHLT | — |
| 225 | timer_getoverrun | FEHLT | — |
| 226 | timer_delete | FEHLT | — |
| 227 | clock_settime | FEHLT | — |
| 228 | clock_gettime | IMPL | src/kernel/syscall/sys_time.c |
| 229 | clock_getres | IMPL | src/kernel/syscall/sys_time.c |
| 230 | clock_nanosleep | IMPL | src/kernel/syscall/sys_time.c |
| 231 | exit_group | IMPL | src/kernel/syscall/sys_proc.c |
| 232 | epoll_wait | IMPL | src/kernel/event/epoll.c |
| 233 | epoll_ctl | IMPL | src/kernel/event/epoll.c |
| 234 | tgkill | IMPL | src/kernel/syscall/sys_signal.c |
| 235 | utimes | FEHLT | — |
| 236 | vserver | FEHLT | — |
| 237 | mbind | FEHLT | — |
| 238 | set_mempolicy | FEHLT | — |
| 239 | get_mempolicy | FEHLT | — |
| 240 | mq_open | FEHLT | — |
| 241 | mq_unlink | FEHLT | — |
| 242 | mq_timedsend | FEHLT | — |
| 243 | mq_timedreceive | FEHLT | — |
| 244 | mq_notify | FEHLT | — |
| 245 | mq_getsetattr | FEHLT | — |
| 246 | kexec_load | FEHLT | — |
| 247 | waitid | FEHLT | — |
| 248 | add_key | FEHLT | — |
| 249 | request_key | FEHLT | — |
| 250 | keyctl | FEHLT | — |
| 251 | ioprio_set | FEHLT | — |
| 252 | ioprio_get | FEHLT | — |
| 253 | inotify_init | FEHLT | — |
| 254 | inotify_add_watch | IMPL | src/kernel/event/epoll.c |
| 255 | inotify_rm_watch | IMPL | src/kernel/event/epoll.c |
| 256 | migrate_pages | FEHLT | — |
| 257 | openat | IMPL | src/kernel/syscall/sys_file.c |
| 258 | mkdirat | IMPL | src/kernel/syscall/sys_file.c |
| 259 | mknodat | IMPL | src/kernel/syscall/sys_file.c |
| 260 | fchownat | FEHLT | — |
| 261 | futimesat | FEHLT | — |
| 262 | newfstatat | IMPL | src/kernel/syscall/sys_file.c |
| 263 | unlinkat | IMPL | src/kernel/syscall/sys_file.c |
| 264 | renameat | FEHLT | — |
| 265 | linkat | IMPL | src/kernel/syscall/sys_file.c |
| 266 | symlinkat | IMPL | src/kernel/syscall/sys_file.c |
| 267 | readlinkat | IMPL | src/kernel/syscall/sys_file.c |
| 268 | fchmodat | IMPL | src/kernel/syscall/sys_file.c |
| 269 | faccessat | IMPL | src/kernel/syscall/sys_file.c |
| 270 | pselect6 | IMPL | src/kernel/syscall/dispatch.c |
| 271 | ppoll | IMPL | src/kernel/syscall/dispatch.c |
| 272 | unshare | FEHLT | — |
| 273 | set_robust_list | STUB | syscall_table.h (return 0) |
| 274 | get_robust_list | FEHLT | — |
| 275 | splice | FEHLT | — |
| 276 | tee | FEHLT | — |
| 277 | sync_file_range | FEHLT | — |
| 278 | vmsplice | FEHLT | — |
| 279 | move_pages | FEHLT | — |
| 280 | utimensat | IMPL | src/kernel/syscall/sys_file.c |
| 281 | epoll_pwait | IMPL | src/kernel/event/epoll.c |
| 282 | signalfd | FEHLT | — |
| 283 | timerfd_create | IMPL | src/kernel/event/epoll.c |
| 284 | eventfd | FEHLT | — |
| 285 | fallocate | IMPL | src/kernel/syscall/sys_file.c |
| 286 | timerfd_settime | IMPL | src/kernel/event/epoll.c |
| 287 | timerfd_gettime | FEHLT | — |
| 288 | accept4 | IMPL | src/kernel/syscall/dispatch.c |
| 289 | signalfd4 | IMPL | src/kernel/event/epoll.c |
| 290 | eventfd2 | IMPL | src/kernel/event/epoll.c |
| 291 | epoll_create1 | IMPL | src/kernel/event/epoll.c |
| 292 | dup3 | IMPL | src/kernel/syscall/sys_file.c |
| 293 | pipe2 | IMPL | src/kernel/syscall/sys_ipc.c |
| 294 | inotify_init1 | IMPL | src/kernel/event/epoll.c |
| 295 | preadv | IMPL | src/kernel/syscall/dispatch.c |
| 296 | pwritev | IMPL | src/kernel/syscall/dispatch.c |
| 297 | rt_tgsigqueueinfo | FEHLT | — |
| 298 | perf_event_open | FEHLT | — |
| 299 | recvmmsg | IMPL | src/kernel/syscall/dispatch.c |
| 300 | fanotify_init | FEHLT | — |
| 301 | fanotify_mark | FEHLT | — |
| 302 | prlimit64 | IMPL | src/kernel/syscall/sys_proc.c |
| 303 | name_to_handle_at | FEHLT | — |
| 304 | open_by_handle_at | FEHLT | — |
| 305 | clock_adjtime | FEHLT | — |
| 306 | syncfs | FEHLT | — |
| 307 | sendmmsg | IMPL | src/kernel/syscall/dispatch.c |
| 308 | setns | FEHLT | — |
| 309 | getcpu | IMPL | src/kernel/syscall/sys_proc.c |
| 310 | process_vm_readv | FEHLT | — |
| 311 | process_vm_writev | FEHLT | — |
| 312 | kcmp | FEHLT | — |
| 313 | finit_module | FEHLT | — |
| 314 | sched_setattr | FEHLT | — |
| 315 | sched_getattr | FEHLT | — |
| 316 | renameat2 | IMPL | src/kernel/syscall/sys_file.c |
| 317 | seccomp | FEHLT | — |
| 318 | getrandom | IMPL | src/kernel/syscall/sys_proc.c |
| 319 | memfd_create | FEHLT | — |
| 320 | kexec_file_load | FEHLT | — |
| 321 | bpf | FEHLT | — |
| 322 | execveat | FEHLT | — |
| 323 | userfaultfd | FEHLT | — |
| 324 | membarrier | FEHLT | — |
| 325 | mlock2 | FEHLT | — |
| 326 | copy_file_range | FEHLT | — |
| 327 | preadv2 | FEHLT | — |
| 328 | pwritev2 | FEHLT | — |
| 329 | pkey_mprotect | FEHLT | — |
| 330 | pkey_alloc | FEHLT | — |
| 331 | pkey_free | FEHLT | — |
| 332 | statx | IMPL | src/kernel/syscall/sys_file.c |
| 333 | io_pgetevents | FEHLT | — |
| 334 | rseq | STUB | syscall_table.h (return -ENOSYS) |
| 335 | uretprobe | FEHLT | — |
| 336 | uprobe | FEHLT | — |
| 424 | pidfd_send_signal | FEHLT | — |
| 425 | io_uring_setup | FEHLT | — |
| 426 | io_uring_enter | FEHLT | — |
| 427 | io_uring_register | FEHLT | — |
| 428 | open_tree | FEHLT | — |
| 429 | move_mount | FEHLT | — |
| 430 | fsopen | FEHLT | — |
| 431 | fsconfig | FEHLT | — |
| 432 | fsmount | FEHLT | — |
| 433 | fspick | FEHLT | — |
| 434 | pidfd_open | FEHLT | — |
| 435 | clone3 | IMPL | src/kernel/syscall/sys_proc.c |
| 436 | close_range | FEHLT | — |
| 437 | openat2 | FEHLT | — |
| 438 | pidfd_getfd | FEHLT | — |
| 439 | faccessat2 | FEHLT | — |
| 440 | process_madvise | FEHLT | — |
| 441 | epoll_pwait2 | FEHLT | — |
| 442 | mount_setattr | FEHLT | — |
| 443 | quotactl_fd | FEHLT | — |
| 444 | landlock_create_ruleset | FEHLT | — |
| 445 | landlock_add_rule | FEHLT | — |
| 446 | landlock_restrict_self | FEHLT | — |
| 447 | memfd_secret | FEHLT | — |
| 448 | process_mrelease | FEHLT | — |
| 449 | futex_waitv | FEHLT | — |
| 450 | set_mempolicy_home_node | FEHLT | — |
| 451 | cachestat | FEHLT | — |
| 452 | fchmodat2 | FEHLT | — |
| 453 | map_shadow_stack | FEHLT | — |
| 454 | futex_wake | FEHLT | — |
| 455 | futex_wait | FEHLT | — |
| 456 | futex_requeue | FEHLT | — |
| 457 | statmount | FEHLT | — |
| 458 | listmount | FEHLT | — |
| 459 | lsm_get_self_attr | FEHLT | — |
| 460 | lsm_set_self_attr | FEHLT | — |
| 461 | lsm_list_modules | FEHLT | — |
| 462 | mseal | FEHLT | — |
| 463 | setxattrat | FEHLT | — |
| 464 | getxattrat | FEHLT | — |
| 465 | listxattrat | FEHLT | — |
| 466 | removexattrat | FEHLT | — |
| 467 | open_tree_attr | FEHLT | — |
| 468 | file_getattr | FEHLT | — |
| 469 | file_setattr | FEHLT | — |
| 470 | listns | FEHLT | — |
| 471 | rseq_slice_yield | FEHLT | — |

## CosmoRT-eigene Syscalls (512+)

| Nr | Name | Status | Datei |
|----|------|--------|-------|
| 512 | cosmo_mmio_map | IMPL | src/kernel/syscall/dispatch.c |
| 513 | cosmo_dma_alloc | IMPL | src/kernel/syscall/dispatch.c |
| 514 | cosmo_dma_free | IMPL | src/kernel/syscall/dispatch.c |
| 515 | cosmo_irq_register | IMPL | src/kernel/syscall/dispatch.c |
| 516 | cosmo_pci_read | IMPL | src/kernel/syscall/dispatch.c |
| 517 | cosmo_pci_write | IMPL | src/kernel/syscall/dispatch.c |
| 518 | cosmo_fw_load | IMPL | src/kernel/syscall/dispatch.c |
| 519 | cosmo_nic_attach | IMPL | src/kernel/syscall/dispatch.c |
| 520 | cosmo_kexec | IMPL | src/kernel/syscall/dispatch.c |

## Zusammenfassung

| Status | Anzahl |
|--------|--------|
| IMPL | 144 |
| STUB | 32 |
| FEHLT | 209 |
| **Gesamt (Linux 0-471)** | **385** |
| CosmoRT-eigen (512+) | 9 IMPL |
