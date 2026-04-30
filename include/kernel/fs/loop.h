/* CosmoRT Loop-Device-Subsystem
 *
 * Provides /dev/loop-control and /dev/loopN block-device facade so
 * LTP's tst_acquire_device__() / tst_find_free_loopdev() / tst_attach_device()
 * can run.
 *
 * Backing-file IO durch FD_DEVICE-Pfad in sys_file. Mount-Integration
 * fehlt noch — mount("/dev/loopN", ...) returnt -ENODEV via existing
 * do_mount tmpfs-only-Logik. Acquire-Pfad selbst ist trotzdem grün.
 *
 * Object-IDs für FD_DEVICE.obj sind in einer eigenen Range, getrennt von
 * DEV_NULL/DEV_ZERO/... (1..4):
 *   DEV_LOOP_CTL       = 100
 *   DEV_LOOP_BASE      = 200..200+LOOP_NDEV-1
 */
#ifndef LOOP_H
#define LOOP_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

#define LOOP_NDEV          8

#define DEV_LOOP_CTL       100
#define DEV_LOOP_BASE      200
#define DEV_LOOP_END       (DEV_LOOP_BASE + LOOP_NDEV)

/* Linux ABI — must not change */
#define LOOP_SET_FD        0x4C00
#define LOOP_CLR_FD        0x4C01
#define LOOP_SET_STATUS    0x4C02
#define LOOP_GET_STATUS    0x4C03
#define LOOP_SET_STATUS64  0x4C04
#define LOOP_GET_STATUS64  0x4C05
#define LOOP_CHANGE_FD     0x4C06
#define LOOP_SET_CAPACITY  0x4C07
#define LOOP_SET_DIRECT_IO 0x4C08
#define LOOP_SET_BLOCK_SIZE 0x4C09
#define LOOP_CTL_ADD       0x4C80
#define LOOP_CTL_REMOVE    0x4C81
#define LOOP_CTL_GET_FREE  0x4C82

/* Linux loop_info / loop_info64 struct sizes (binary compatibility) */
struct loop_info {
    int    lo_number;
    uint32_t lo_device;
    uint64_t lo_inode;
    uint32_t lo_rdevice;
    int    lo_offset;
    int    lo_encrypt_type;
    int    lo_encrypt_key_size;
    int    lo_flags;
    char   lo_name[64];
    unsigned char lo_encrypt_key[32];
    uint32_t lo_init[2];
    char   reserved[4];
};

struct loop_info64 {
    uint64_t lo_device;
    uint64_t lo_inode;
    uint64_t lo_rdevice;
    uint64_t lo_offset;
    uint64_t lo_sizelimit;
    uint32_t lo_number;
    uint32_t lo_encrypt_type;
    uint32_t lo_encrypt_key_size;
    uint32_t lo_flags;
    char     lo_file_name[64];
    char     lo_crypt_name[64];
    unsigned char lo_encrypt_key[32];
    uint64_t lo_init[2];
};

struct ext4_fs;
struct bcache_inst;
struct vfs_file;

struct loop_dev {
    int       in_use;     /* slot allocated via LOOP_CTL_GET_FREE */
    int       bound;      /* LOOP_SET_FD done */
    int       open_refs;  /* count of live /dev/loopN fds (Linux lo_refcnt) */
    int       backing_fd; /* O_RDWR fd into backing file (kernel-private dup) */
    struct vfs_file *backing_file;  /* pinned vfs_file* for kernel-side I/O */
    uint64_t  offset;
    uint64_t  sizelimit;
    uint32_t  flags;
    char      lo_name[64];
    spinlock_t lock;

    /* Mounted-on-this-loop ext4 fs (set by do_mount, cleared by do_umount2) */
    struct ext4_fs    *mounted_fs;
    struct bcache_inst *mounted_bcache;
    int                mounted;
};

/* Init at vfs_init() time. */
void loop_init(void);

/* ioctl dispatch — called from sys_file::do_ioctl when FD_DEVICE is a loop. */
long loop_ioctl(int devid, unsigned long request, unsigned long arg);

/* Backing-file IO for read/write on /dev/loopN */
long loop_read(int devid, void *user_buf, size_t count, int64_t *file_pos);
long loop_write(int devid, const void *user_buf, size_t count, int64_t *file_pos);

/* open(2) on /dev/loopN: bump open_refs for refcount-based slot lifetime. */
void loop_dev_open(int devid);

/* close(2): decrement open_refs. If zero and not bound, free the slot
 * (in_use=0) so LOOP_CTL_GET_FREE can hand it out again. Without this the
 * 8-slot pool exhausts after the first 8 LTP tests that acquire devices. */
void loop_dev_release(int devid);

/* Lookup info — used by stat / open path / mount lookup. */
struct loop_dev *loop_dev_get(int idx);

/* Get loop-device index from /dev/loopN path. Returns -1 if not a loopN path. */
int loop_path_to_idx(const char *path);

/* bcache backend ops for a bound loop device — exposed for do_mount. */
int loop_bcache_read(void *ctx, uint64_t block, void *buf);
int loop_bcache_write(void *ctx, uint64_t block, const void *buf);

#endif
