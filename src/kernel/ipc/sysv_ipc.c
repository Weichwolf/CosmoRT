/* CosmoRT — SysV IPC: message queues, semaphores, shared memory */

#include "ipc/ipc.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/mutex.h"
#include "mm/page_alloc.h"
#include "memops.h"
#include "uaccess.h"
#include "mm/vma.h"
#include "arch/arch.h"
#include "linux/abi.h"

#define PTE_PRESENT (1ULL << 0)
#define PTE_PS      (1ULL << 7)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define IPC_CREAT    01000
#define IPC_EXCL     02000
#define IPC_NOWAIT   04000
#define IPC_RMID     0
#define IPC_SET      1
#define IPC_STAT     2
#define IPC_PRIVATE  0
#define IPC_64       0x0100

#define MSG_NOERROR  010000
#define MSG_EXCEPT   020000

#define SHM_RDONLY   010000

#define GETPID   11
#define GETVAL   12
#define GETALL   13
#define GETNCNT  14
#define GETZCNT  15
#define SETVAL   16
#define SETALL   17

extern uint64_t timer_ms(void);
extern uint64_t rtc_epoch_sec;

static int64_t current_time_sec(void) {
    return (int64_t)(timer_ms() / 1000) + (int64_t)rtc_epoch_sec;
}

static int32_t current_pid(void) {
    process_t *p = proc_current();
    return p ? (int32_t)p->pid : 0;
}

struct k_ipc_perm {
    int32_t  key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    int32_t  seq;
    int64_t  __pad1;
    int64_t  __pad2;
};

#define SYSV_MSG_MAX  32
#define MSG_QBYTES    16384
#define MSG_MAX_SIZE  8192

typedef struct sysv_msg {
    struct sysv_msg *next;
    long   type;
    size_t size;
    char   data[];
} sysv_msg_t;

struct k_msqid_ds {
    struct k_ipc_perm msg_perm;
    int64_t  msg_stime;
    int64_t  msg_rtime;
    int64_t  msg_ctime;
    uint64_t msg_cbytes;
    uint64_t msg_qnum;
    uint64_t msg_qbytes;
    int32_t  msg_lspid;
    int32_t  msg_lrpid;
    uint64_t __unused[2];
};

typedef struct {
    int        used;
    int32_t    key;
    sysv_msg_t *head;
    sysv_msg_t *tail;
    uint64_t   cbytes;
    uint64_t   qnum;
    uint64_t   qbytes;
    int32_t    lspid;
    int32_t    lrpid;
    int64_t    stime;
    int64_t    rtime;
    int64_t    ctime;
    uint32_t   mode;
    uint32_t   uid, gid, cuid, cgid;
} msgq_t;

static msgq_t msg_table[SYSV_MSG_MAX];
static mutex_t msg_lock = MUTEX_INIT;

long do_msgget(int32_t key, int flags) {
    mutex_lock(&msg_lock);

    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SYSV_MSG_MAX; i++) {
            if (msg_table[i].used && msg_table[i].key == key) {
                if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) {
                    mutex_unlock(&msg_lock);
                    return -EEXIST;
                }
                mutex_unlock(&msg_lock);
                return i;
            }
        }
    }

    if (!(flags & IPC_CREAT) && key != IPC_PRIVATE) {
        mutex_unlock(&msg_lock);
        return -ENOENT;
    }

    for (int i = 0; i < SYSV_MSG_MAX; i++) {
        if (!msg_table[i].used) {
            msgq_t *q = &msg_table[i];
            q->used = 1;
            q->key = key;
            q->head = q->tail = (void *)0;
            q->cbytes = 0;
            q->qnum = 0;
            q->qbytes = MSG_QBYTES;
            q->lspid = q->lrpid = 0;
            q->stime = q->rtime = 0;
            q->ctime = current_time_sec();
            q->mode = flags & 0x1FF;
            q->uid = q->cuid = 0;
            q->gid = q->cgid = 0;
            mutex_unlock(&msg_lock);
            return i;
        }
    }

    mutex_unlock(&msg_lock);
    return -ENOSPC;
}

long do_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg) {
    if (msqid < 0 || msqid >= SYSV_MSG_MAX) return -EINVAL;
    if (msgsz > MSG_MAX_SIZE) return -EINVAL;

    long mtype;
    int r = copy_from_user(&mtype, msgp, sizeof(long));
    if (r) return r;
    if (mtype < 1) return -EINVAL;

    mutex_lock(&msg_lock);

    msgq_t *q = &msg_table[msqid];
    if (!q->used) {
        mutex_unlock(&msg_lock);
        return -EINVAL;
    }

    if (q->cbytes + msgsz > q->qbytes) {
        mutex_unlock(&msg_lock);
        if (msgflg & IPC_NOWAIT) return -EAGAIN;
        return -EAGAIN;
    }

    size_t alloc_sz = sizeof(sysv_msg_t) + msgsz;
    int npages = (int)((alloc_sz + 4095) / 4096);
    sysv_msg_t *m = (sysv_msg_t *)pages_alloc(npages);
    if (!m) {
        mutex_unlock(&msg_lock);
        return -ENOMEM;
    }

    m->type = mtype;
    m->size = msgsz;
    m->next = (void *)0;

    if (msgsz > 0) {
        r = copy_from_user(m->data, (const char *)msgp + sizeof(long), msgsz);
        if (r) {
            pages_free(m, npages);
            mutex_unlock(&msg_lock);
            return r;
        }
    }

    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
    q->cbytes += msgsz;
    q->qnum++;
    q->lspid = current_pid();
    q->stime = current_time_sec();

    mutex_unlock(&msg_lock);
    return 0;
}

long do_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg) {
    if (msqid < 0 || msqid >= SYSV_MSG_MAX) return -EINVAL;

    mutex_lock(&msg_lock);

    msgq_t *q = &msg_table[msqid];
    if (!q->used) {
        mutex_unlock(&msg_lock);
        return -EINVAL;
    }

    sysv_msg_t *prev = (void *)0;
    sysv_msg_t *m = q->head;
    while (m) {
        int match = 0;
        if (msgtyp == 0) match = 1;
        else if (msgtyp > 0 && m->type == msgtyp) match = 1;
        else if (msgtyp < 0 && m->type <= -msgtyp) match = 1;
        if (match) break;
        prev = m;
        m = m->next;
    }

    if (!m) {
        mutex_unlock(&msg_lock);
        if (msgflg & IPC_NOWAIT) return -ENOMSG;
        return -ENOMSG;
    }

    if (m->size > msgsz) {
        if (!(msgflg & MSG_NOERROR)) {
            mutex_unlock(&msg_lock);
            return -E2BIG;
        }
    }

    if (prev)
        prev->next = m->next;
    else
        q->head = m->next;
    if (q->tail == m)
        q->tail = prev;
    q->cbytes -= m->size;
    q->qnum--;
    q->lrpid = current_pid();
    q->rtime = current_time_sec();

    mutex_unlock(&msg_lock);

    size_t copy_sz = m->size < msgsz ? m->size : msgsz;
    int r = copy_to_user(msgp, &m->type, sizeof(long));
    if (!r && copy_sz > 0)
        r = copy_to_user((char *)msgp + sizeof(long), m->data, copy_sz);

    long ret = r ? r : (long)copy_sz;

    size_t alloc_sz = sizeof(sysv_msg_t) + m->size;
    int npages = (int)((alloc_sz + 4095) / 4096);
    pages_free(m, npages);

    return ret;
}

long do_msgctl(int msqid, int cmd, void *buf) {
    cmd &= ~IPC_64;
    if (msqid < 0 || msqid >= SYSV_MSG_MAX) return -EINVAL;

    mutex_lock(&msg_lock);

    msgq_t *q = &msg_table[msqid];
    if (!q->used) {
        mutex_unlock(&msg_lock);
        return -EINVAL;
    }

    switch (cmd) {
    case IPC_RMID: {
        sysv_msg_t *m = q->head;
        while (m) {
            sysv_msg_t *next = m->next;
            size_t alloc_sz = sizeof(sysv_msg_t) + m->size;
            int npages = (int)((alloc_sz + 4095) / 4096);
            pages_free(m, npages);
            m = next;
        }
        q->used = 0;
        mutex_unlock(&msg_lock);
        return 0;
    }
    case IPC_STAT: {
        if (!buf) {
            mutex_unlock(&msg_lock);
            return -EFAULT;
        }
        struct k_msqid_ds ds;
        kmemset(&ds, 0, sizeof(ds));
        ds.msg_perm.key = q->key;
        ds.msg_perm.uid = q->uid;
        ds.msg_perm.gid = q->gid;
        ds.msg_perm.cuid = q->cuid;
        ds.msg_perm.cgid = q->cgid;
        ds.msg_perm.mode = q->mode;
        ds.msg_stime = q->stime;
        ds.msg_rtime = q->rtime;
        ds.msg_ctime = q->ctime;
        ds.msg_cbytes = q->cbytes;
        ds.msg_qnum = q->qnum;
        ds.msg_qbytes = q->qbytes;
        ds.msg_lspid = q->lspid;
        ds.msg_lrpid = q->lrpid;
        mutex_unlock(&msg_lock);
        return copy_to_user(buf, &ds, sizeof(ds));
    }
    case IPC_SET: {
        if (!buf) {
            mutex_unlock(&msg_lock);
            return -EFAULT;
        }
        struct k_msqid_ds ds;
        int r = copy_from_user(&ds, buf, sizeof(ds));
        if (r) {
            mutex_unlock(&msg_lock);
            return r;
        }
        q->uid = ds.msg_perm.uid;
        q->gid = ds.msg_perm.gid;
        q->mode = ds.msg_perm.mode & 0x1FF;
        q->qbytes = ds.msg_qbytes;
        q->ctime = current_time_sec();
        mutex_unlock(&msg_lock);
        return 0;
    }
    default:
        mutex_unlock(&msg_lock);
        return -EINVAL;
    }
}

#define SYSV_SEM_MAX     32
#define SYSV_NSEMS_MAX   64

struct k_semid_ds {
    struct k_ipc_perm sem_perm;
    int64_t  sem_otime;
    int64_t  __unused1;
    int64_t  sem_ctime;
    int64_t  __unused2;
    uint16_t sem_nsems;
    char     __pad[sizeof(long) - sizeof(uint16_t)];
    int64_t  __unused3;
    int64_t  __unused4;
};

struct k_sembuf {
    uint16_t sem_num;
    int16_t  sem_op;
    int16_t  sem_flg;
};

typedef struct {
    int      used;
    int32_t  key;
    uint16_t nsems;
    int16_t  vals[SYSV_NSEMS_MAX];
    int32_t  pids[SYSV_NSEMS_MAX];
    int64_t  otime;
    int64_t  ctime;
    uint32_t mode;
    uint32_t uid, gid, cuid, cgid;
} semset_t;

static semset_t sem_table[SYSV_SEM_MAX];
static mutex_t sem_lock = MUTEX_INIT;

long do_semget(int32_t key, int nsems, int flags) {
    if (nsems < 0 || nsems > SYSV_NSEMS_MAX) return -EINVAL;

    mutex_lock(&sem_lock);

    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SYSV_SEM_MAX; i++) {
            if (sem_table[i].used && sem_table[i].key == key) {
                if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) {
                    mutex_unlock(&sem_lock);
                    return -EEXIST;
                }
                mutex_unlock(&sem_lock);
                return i;
            }
        }
    }

    if (!(flags & IPC_CREAT) && key != IPC_PRIVATE) {
        mutex_unlock(&sem_lock);
        return -ENOENT;
    }

    if (nsems == 0) nsems = 1;

    for (int i = 0; i < SYSV_SEM_MAX; i++) {
        if (!sem_table[i].used) {
            semset_t *s = &sem_table[i];
            s->used = 1;
            s->key = key;
            s->nsems = (uint16_t)nsems;
            for (int j = 0; j < nsems; j++) {
                s->vals[j] = 0;
                s->pids[j] = 0;
            }
            s->otime = 0;
            s->ctime = current_time_sec();
            s->mode = flags & 0x1FF;
            s->uid = s->cuid = 0;
            s->gid = s->cgid = 0;
            mutex_unlock(&sem_lock);
            return i;
        }
    }

    mutex_unlock(&sem_lock);
    return -ENOSPC;
}

long do_semop(int semid, const void *usops, size_t nsops) {
    if (semid < 0 || semid >= SYSV_SEM_MAX) return -EINVAL;
    if (nsops == 0 || nsops > 32) return -EINVAL;

    struct k_sembuf sops[32];
    int r = copy_from_user(sops, usops, nsops * sizeof(struct k_sembuf));
    if (r) return r;

    mutex_lock(&sem_lock);

    semset_t *s = &sem_table[semid];
    if (!s->used) {
        mutex_unlock(&sem_lock);
        return -EINVAL;
    }

    for (size_t i = 0; i < nsops; i++) {
        if (sops[i].sem_num >= s->nsems) {
            mutex_unlock(&sem_lock);
            return -EFBIG;
        }
    }

    int16_t tmp[SYSV_NSEMS_MAX];
    for (int j = 0; j < s->nsems; j++)
        tmp[j] = s->vals[j];

    for (size_t i = 0; i < nsops; i++) {
        int idx = sops[i].sem_num;
        int16_t op = sops[i].sem_op;
        if (op > 0) {
            tmp[idx] += op;
        } else if (op < 0) {
            if (tmp[idx] + op < 0) {
                mutex_unlock(&sem_lock);
                if (sops[i].sem_flg & IPC_NOWAIT) return -EAGAIN;
                return -EAGAIN;
            }
            tmp[idx] += op;
        } else {
            if (tmp[idx] != 0) {
                mutex_unlock(&sem_lock);
                if (sops[i].sem_flg & IPC_NOWAIT) return -EAGAIN;
                return -EAGAIN;
            }
        }
    }

    int32_t pid = current_pid();
    for (size_t i = 0; i < nsops; i++) {
        int idx = sops[i].sem_num;
        s->vals[idx] = tmp[idx];
        s->pids[idx] = pid;
    }
    s->otime = current_time_sec();

    mutex_unlock(&sem_lock);
    return 0;
}

long do_semctl(int semid, int semnum, int cmd, long arg4) {
    cmd &= ~IPC_64;
    if (semid < 0 || semid >= SYSV_SEM_MAX) return -EINVAL;

    mutex_lock(&sem_lock);

    semset_t *s = &sem_table[semid];
    if (!s->used) {
        mutex_unlock(&sem_lock);
        return -EINVAL;
    }

    switch (cmd) {
    case IPC_RMID:
        s->used = 0;
        mutex_unlock(&sem_lock);
        return 0;

    case GETVAL:
        if (semnum < 0 || semnum >= s->nsems) {
            mutex_unlock(&sem_lock);
            return -EINVAL;
        }
        { long v = s->vals[semnum]; mutex_unlock(&sem_lock); return v; }

    case SETVAL:
        if (semnum < 0 || semnum >= s->nsems) {
            mutex_unlock(&sem_lock);
            return -EINVAL;
        }
        s->vals[semnum] = (int16_t)arg4;
        s->ctime = current_time_sec();
        mutex_unlock(&sem_lock);
        return 0;

    case GETPID:
        if (semnum < 0 || semnum >= s->nsems) {
            mutex_unlock(&sem_lock);
            return -EINVAL;
        }
        { long p = s->pids[semnum]; mutex_unlock(&sem_lock); return p; }

    case GETNCNT:
        if (semnum < 0 || semnum >= s->nsems) {
            mutex_unlock(&sem_lock);
            return -EINVAL;
        }
        mutex_unlock(&sem_lock);
        return 0;

    case GETZCNT:
        if (semnum < 0 || semnum >= s->nsems) {
            mutex_unlock(&sem_lock);
            return -EINVAL;
        }
        mutex_unlock(&sem_lock);
        return 0;

    case IPC_STAT: {
        struct k_semid_ds *ubuf = (struct k_semid_ds *)(uintptr_t)arg4;
        if (!ubuf) {
            mutex_unlock(&sem_lock);
            return -EFAULT;
        }
        struct k_semid_ds ds;
        kmemset(&ds, 0, sizeof(ds));
        ds.sem_perm.key = s->key;
        ds.sem_perm.uid = s->uid;
        ds.sem_perm.gid = s->gid;
        ds.sem_perm.cuid = s->cuid;
        ds.sem_perm.cgid = s->cgid;
        ds.sem_perm.mode = s->mode;
        ds.sem_otime = s->otime;
        ds.sem_ctime = s->ctime;
        ds.sem_nsems = s->nsems;
        mutex_unlock(&sem_lock);
        return copy_to_user(ubuf, &ds, sizeof(ds));
    }

    case IPC_SET: {
        struct k_semid_ds *ubuf = (struct k_semid_ds *)(uintptr_t)arg4;
        if (!ubuf) {
            mutex_unlock(&sem_lock);
            return -EFAULT;
        }
        struct k_semid_ds ds;
        int r2 = copy_from_user(&ds, ubuf, sizeof(ds));
        if (r2) {
            mutex_unlock(&sem_lock);
            return r2;
        }
        s->uid = ds.sem_perm.uid;
        s->gid = ds.sem_perm.gid;
        s->mode = ds.sem_perm.mode & 0x1FF;
        s->ctime = current_time_sec();
        mutex_unlock(&sem_lock);
        return 0;
    }

    default:
        mutex_unlock(&sem_lock);
        return -EINVAL;
    }
}

#define SYSV_SHM_MAX     32
#define SHM_MAX_SIZE     (16 * 1024 * 1024)

struct k_shmid_ds {
    struct k_ipc_perm shm_perm;
    uint64_t shm_segsz;
    int64_t  shm_atime;
    int64_t  shm_dtime;
    int64_t  shm_ctime;
    int32_t  shm_cpid;
    int32_t  shm_lpid;
    uint64_t shm_nattch;
    uint64_t __pad1;
    uint64_t __pad2;
};

typedef struct {
    int      used;
    int32_t  key;
    uint64_t size;
    int      npages;
    void    *pages;
    uint64_t nattch;
    int32_t  cpid;
    int32_t  lpid;
    int64_t  atime;
    int64_t  dtime;
    int64_t  ctime;
    uint32_t mode;
    uint32_t uid, gid, cuid, cgid;
} shmseg_t;

static shmseg_t shm_table[SYSV_SHM_MAX];
static mutex_t shm_lock = MUTEX_INIT;

long do_shmget(int32_t key, size_t size, int flags) {
    mutex_lock(&shm_lock);

    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (shm_table[i].used && shm_table[i].key == key) {
                if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) {
                    mutex_unlock(&shm_lock);
                    return -EEXIST;
                }
                mutex_unlock(&shm_lock);
                return i;
            }
        }
    }

    if (!(flags & IPC_CREAT) && key != IPC_PRIVATE) {
        mutex_unlock(&shm_lock);
        return -ENOENT;
    }

    if (size > SHM_MAX_SIZE) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }

    int npages = (int)((size + 4095) / 4096);
    if (npages == 0) npages = 1;

    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (!shm_table[i].used) {
            void *p = pages_alloc(npages);
            if (!p) {
                mutex_unlock(&shm_lock);
                return -ENOMEM;
            }
            shmseg_t *seg = &shm_table[i];
            seg->used = 1;
            seg->key = key;
            seg->size = size;
            seg->npages = npages;
            seg->pages = p;
            seg->nattch = 0;
            seg->cpid = current_pid();
            seg->lpid = 0;
            seg->atime = seg->dtime = 0;
            seg->ctime = current_time_sec();
            seg->mode = flags & 0x1FF;
            seg->uid = seg->cuid = 0;
            seg->gid = seg->cgid = 0;
            mutex_unlock(&shm_lock);
            return i;
        }
    }

    mutex_unlock(&shm_lock);
    return -ENOSPC;
}

long do_shmat(int shmid, const void *shmaddr, int shmflg) {
    if (shmid < 0 || shmid >= SYSV_SHM_MAX) return -EINVAL;

    mutex_lock(&shm_lock);

    shmseg_t *seg = &shm_table[shmid];
    if (!seg->used) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }

    process_t *proc = proc_current();
    if (!proc) {
        mutex_unlock(&shm_lock);
        return -ESRCH;
    }

    int prot = PROT_READ;
    if (!(shmflg & SHM_RDONLY)) prot |= PROT_WRITE;
    int map_flags = MAP_SHARED | MAP_ANONYMOUS;

    uint64_t map_size = (uint64_t)seg->npages * 4096;

    uint64_t base;
    if (shmaddr) {
        base = (uint64_t)shmaddr & ~0xFFFULL;
    } else {
        base = vma_find_free(proc->vma_root, 0x7F0000000000ULL, map_size);
        if (!base) {
            mutex_unlock(&shm_lock);
            return -ENOMEM;
        }
    }

    vma_t *v = vma_insert(&proc->vma_root, base, base + map_size, prot, map_flags);
    if (!v) {
        mutex_unlock(&shm_lock);
        return -ENOMEM;
    }

    uint64_t phys_base = virt_to_phys(seg->pages);
    for (int i = 0; i < seg->npages; i++) {
        uint64_t phys = phys_base + (uint64_t)i * 4096;
        page_incref(phys);
        map_user_page(proc->pml4, base + (uint64_t)i * 4096, phys, prot);
    }

    seg->nattch++;
    seg->lpid = current_pid();
    seg->atime = current_time_sec();

    mutex_unlock(&shm_lock);
    return (long)base;
}

long do_shmdt(const void *shmaddr) {
    if (!shmaddr) return -EINVAL;
    uint64_t addr = (uint64_t)shmaddr;

    process_t *proc = proc_current();
    if (!proc) return -ESRCH;

    vma_t *v = vma_find(proc->vma_root, addr);
    if (!v || v->start != addr) return -EINVAL;

    uint64_t size = v->end - v->start;

    mutex_lock(&shm_lock);

    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        shmseg_t *seg = &shm_table[i];
        if (!seg->used) continue;
        uint64_t map_size = (uint64_t)seg->npages * 4096;
        if (map_size == size) {
            if (seg->nattch > 0) seg->nattch--;
            seg->lpid = current_pid();
            seg->dtime = current_time_sec();
            break;
        }
    }

    mutex_unlock(&shm_lock);

    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL;
    uint64_t *pml4 = proc->pml4;
    for (uint64_t va = addr; va < addr + size; va += 4096) {
        int pml4i = (va >> 39) & 0x1FF;
        if (!(pml4[pml4i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[pml4i] & PHYS_MASK);
        int pdpti = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpti] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[pdpti] & PHYS_MASK);
        int pdi = (va >> 21) & 0x1FF;
        if (!(pd[pdi] & PTE_PRESENT)) continue;
        if (pd[pdi] & PTE_PS) continue;
        uint64_t *pt = (uint64_t *)phys_to_virt(pd[pdi] & PHYS_MASK);
        int pti = (va >> 12) & 0x1FF;
        if (pt[pti] & PTE_PRESENT) {
            uint64_t phys = pt[pti] & PHYS_MASK;
            pt[pti] = 0;
            page_free(phys_to_virt(phys));
            arch_invlpg(va);
        }
    }

    vma_remove(&proc->vma_root, v);

    return 0;
}

long do_shmctl(int shmid, int cmd, void *buf) {
    cmd &= ~IPC_64;
    if (shmid < 0 || shmid >= SYSV_SHM_MAX) return -EINVAL;

    mutex_lock(&shm_lock);

    shmseg_t *seg = &shm_table[shmid];
    if (!seg->used) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }

    switch (cmd) {
    case IPC_RMID:
        if (seg->nattch == 0) {
            pages_free(seg->pages, seg->npages);
        }
        seg->used = 0;
        mutex_unlock(&shm_lock);
        return 0;

    case IPC_STAT: {
        if (!buf) {
            mutex_unlock(&shm_lock);
            return -EFAULT;
        }
        struct k_shmid_ds ds;
        kmemset(&ds, 0, sizeof(ds));
        ds.shm_perm.key = seg->key;
        ds.shm_perm.uid = seg->uid;
        ds.shm_perm.gid = seg->gid;
        ds.shm_perm.cuid = seg->cuid;
        ds.shm_perm.cgid = seg->cgid;
        ds.shm_perm.mode = seg->mode;
        ds.shm_segsz = seg->size;
        ds.shm_atime = seg->atime;
        ds.shm_dtime = seg->dtime;
        ds.shm_ctime = seg->ctime;
        ds.shm_cpid = seg->cpid;
        ds.shm_lpid = seg->lpid;
        ds.shm_nattch = seg->nattch;
        mutex_unlock(&shm_lock);
        return copy_to_user(buf, &ds, sizeof(ds));
    }

    case IPC_SET: {
        if (!buf) {
            mutex_unlock(&shm_lock);
            return -EFAULT;
        }
        struct k_shmid_ds ds;
        int r = copy_from_user(&ds, buf, sizeof(ds));
        if (r) {
            mutex_unlock(&shm_lock);
            return r;
        }
        seg->uid = ds.shm_perm.uid;
        seg->gid = ds.shm_perm.gid;
        seg->mode = ds.shm_perm.mode & 0x1FF;
        seg->ctime = current_time_sec();
        mutex_unlock(&shm_lock);
        return 0;
    }

    default:
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }
}
