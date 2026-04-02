# Syscall Completeness Audit — Plan

Systematisch jeden implementierten Syscall gegen Linux Man-Page auditieren.
Nicht neue hinzufügen, sondern vorhandene richtig machen.

## Priorität 1: mmap/mprotect/munmap
- ALLE Flags: MAP_NORESERVE, MAP_FIXED_NOREPLACE, MAP_POPULATE, MAP_GROWSDOWN, MAP_HUGETLB, MAP_32BIT, MAP_STACK, MAP_NORESERVE
- PROT_NONE → mprotect(PROT_RW) Sequenz (V8 JIT Pattern)
- mprotect PROT_EXEC nach PROT_WRITE (JIT: write code, then execute)
- munmap partial unmap (split VMA)
- mremap MREMAP_FIXED, MREMAP_MAYMOVE

## Priorität 2: clone/fork/exec
- clone: ALLE Flag-Kombinationen (CLONE_VM, CLONE_VFORK, CLONE_THREAD, CLONE_NEWNS, CLONE_SETTLS, CLONE_PARENT_SETTID, CLONE_CHILD_SETTID, CLONE_CHILD_CLEARTID)
- execve: shebang (#!) Handling, argv/envp Limits
- fork: COW Vollständigkeit, VMA Kopie

## Priorität 3: open/read/write/close
- open: O_DIRECTORY, O_NOATIME, O_PATH, O_TMPFILE, O_NOFOLLOW, O_CLOEXEC, O_NONBLOCK, O_APPEND, O_TRUNC, O_CREAT, O_EXCL
- read/write: partial reads, EINTR, non-blocking, pipe semantics
- pread64/pwrite64: offset handling
- readv/writev: scatter-gather vollständig

## Priorität 4: fcntl/ioctl
- fcntl: F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL, F_SETLK, F_GETLK, F_SETLKW, F_GETOWN, F_SETOWN
- ioctl: TIOCGWINSZ, TIOCSWINSZ, TIOCSCTTY, TIOCNOTTY, TIOCGPGRP, TIOCSPGRP, FIONREAD, FIONBIO, TCGETS, TCSETS, TCSETSW, TCSETSF, TIOCGSID

## Priorität 5: socket
- SO_ERROR, SO_KEEPALIVE, SO_REUSEADDR, SO_REUSEPORT, SO_RCVTIMEO, SO_SNDTIMEO, SO_LINGER, SO_SNDBUF, SO_RCVBUF, SO_BROADCAST, SO_OOBINLINE
- TCP_NODELAY, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT, TCP_CORK, TCP_QUICKACK, TCP_FASTOPEN, TCP_FASTOPEN_CONNECT
- getsockopt/setsockopt: vollständig

## Priorität 6: signal
- SA_ONSTACK, SA_NODEFER, SA_RESETHAND, SA_RESTART, SA_SIGINFO, SA_NOCLDSTOP, SA_NOCLDWAIT
- sigaltstack: vollständig
- Alle Signal-Nummern korrekt dispatched

## Priorität 7: prctl/arch_prctl
- PR_SET_NAME, PR_GET_NAME, PR_SET_PDEATHSIG, PR_GET_PDEATHSIG
- PR_SET_DUMPABLE, PR_SET_NO_NEW_PRIVS
- ARCH_SET_FS, ARCH_GET_FS, ARCH_SET_GS, ARCH_GET_GS
