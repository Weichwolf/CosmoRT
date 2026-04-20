/* Linux x86_64 ABI — epoll/eventfd/timerfd/inotify constants */
#ifndef COSMO_LINUX_EPOLL_H
#define COSMO_LINUX_EPOLL_H

#include "types.h"

#define EPOLLIN         0x001
#define EPOLLOUT        0x004
#define EPOLLERR        0x008
#define EPOLLHUP        0x010
#define EPOLLRDHUP      0x2000
#define EPOLLET         (1U << 31)

#define EPOLL_CLOEXEC   02000000

#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3

#define EFD_SEMAPHORE   00000001
#define EFD_CLOEXEC     02000000
#define EFD_NONBLOCK    04000
#define TFD_CLOEXEC     02000000
#define TFD_NONBLOCK    04000

#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_CLOSE         (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_MOVE          (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_ALL_EVENTS    0x00000FFF
#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_CREATE   0x10000000
#define IN_MASK_ADD      0x20000000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000

#endif /* COSMO_LINUX_EPOLL_H */
