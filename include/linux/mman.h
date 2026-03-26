/* Linux x86_64 ABI — memory mapping flags */
#ifndef COSMO_LINUX_MMAN_H
#define COSMO_LINUX_MMAN_H

#include "types.h"

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED           0x01
#define MAP_PRIVATE          0x02
#define MAP_FIXED            0x10
#define MAP_ANONYMOUS        0x20
#define MAP_NORESERVE        0x4000
#define MAP_POPULATE         0x8000
#define MAP_FIXED_NOREPLACE  0x100000

/* mremap flags */
#define MREMAP_MAYMOVE  1
#define MREMAP_FIXED    2

/* madvise advice values */
#define MADV_NORMAL     0
#define MADV_DONTNEED   4
#define MADV_FREE       8

/* mlockall flags */
#define MCL_CURRENT     1
#define MCL_FUTURE      2

/* msync flags */
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

#endif /* COSMO_LINUX_MMAN_H */
