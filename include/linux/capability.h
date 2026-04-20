/* Linux x86_64 ABI — capability(7) */
#ifndef COSMO_LINUX_CAPABILITY_H
#define COSMO_LINUX_CAPABILITY_H

#include "types.h"

#define _LINUX_CAPABILITY_VERSION_1   0x19980330
#define _LINUX_CAPABILITY_VERSION_2   0x20071026
#define _LINUX_CAPABILITY_VERSION_3   0x20080522

#define _LINUX_CAPABILITY_U32S_1      1
#define _LINUX_CAPABILITY_U32S_2      2
#define _LINUX_CAPABILITY_U32S_3      2

struct __user_cap_header_struct {
    uint32_t version;
    int      pid;
};

struct __user_cap_data_struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

#endif /* COSMO_LINUX_CAPABILITY_H */
