/* Linux x86_64 ABI — prctl and arch_prctl constants */
#ifndef COSMO_LINUX_PRCTL_H
#define COSMO_LINUX_PRCTL_H

#include "types.h"

#define PR_SET_PDEATHSIG    1
#define PR_SET_NAME         15
#define PR_GET_NAME         16

#define ARCH_SET_GS     0x1001
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_GET_GS     0x1004

#endif /* COSMO_LINUX_PRCTL_H */
