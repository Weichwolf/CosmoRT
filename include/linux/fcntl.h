/* Linux x86_64 ABI — open/fcntl flags */
#ifndef COSMO_LINUX_FCNTL_H
#define COSMO_LINUX_FCNTL_H

#include "types.h"

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_ACCMODE       0x0003
#define O_CREAT         0x0040
#define O_EXCL          0x0080
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_NOCTTY        0x100
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_NOATIME       0x40000
#define O_CLOEXEC       0x80000
#define O_PATH          0x200000
#define O_TMPFILE       0x410000  /* O_TMPFILE | O_DIRECTORY */

/* fcntl commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_GETOWN        9
#define F_SETOWN        8
#define F_GETPIPE_SZ    1032
#define F_SETPIPE_SZ    1031
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

/* flock for fcntl F_GETLK/F_SETLK/F_SETLKW */
struct k_flock {
    short l_type;       /* F_RDLCK, F_WRLCK, F_UNLCK */
    short l_whence;     /* SEEK_SET, SEEK_CUR, SEEK_END */
    long  l_start;      /* offset */
    long  l_len;        /* 0 = entire file */
    int   l_pid;        /* PID of lock owner */
};
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

/* flock(2) operations */
#define LOCK_SH  1
#define LOCK_EX  2
#define LOCK_NB  4
#define LOCK_UN  8

/* Seek */
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

/* AT constants */
#define AT_FDCWD              (-100)
#define AT_SYMLINK_NOFOLLOW   0x100
#define AT_REMOVEDIR          0x200
#define AT_SYMLINK_FOLLOW     0x400
#define AT_EMPTY_PATH         0x1000

/* access() mode bits */
#define F_OK    0
#define R_OK    4
#define W_OK    2
#define X_OK    1

/* renameat2 flags */
#define RENAME_NOREPLACE      (1 << 0)
#define RENAME_EXCHANGE       (1 << 1)
#define RENAME_WHITEOUT       (1 << 2)

#endif /* COSMO_LINUX_FCNTL_H */
