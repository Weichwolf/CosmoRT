/* Linux x86_64 ABI — st_mode bits */
#ifndef COSMO_LINUX_STAT_H
#define COSMO_LINUX_STAT_H

#include "types.h"

#define S_IFMT          0170000
#define S_IFCHR         0020000
#define S_IFIFO         0010000
#define S_IFREG         0100000
#define S_IFDIR         0040000
#define S_IFLNK         0120000
#define S_IFSOCK        0140000

#define S_ISUID         04000
#define S_ISGID         02000
#define S_ISVTX         01000

#endif /* COSMO_LINUX_STAT_H */
