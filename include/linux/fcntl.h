/* Linux x86_64 ABI — open/fcntl flags */
#ifndef COSMO_LINUX_FCNTL_H
#define COSMO_LINUX_FCNTL_H

#include "types.h"

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_CREAT         0x0040
#define O_EXCL          0x0080
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_NOCTTY        0x100
#define O_DIRECTORY     0x10000
#define O_NOFOLLOW      0x20000
#define O_CLOEXEC       0x80000

/* fcntl commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

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
