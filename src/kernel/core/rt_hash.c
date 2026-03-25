/* CosmoRT Hash Engine — RT-Core background SHA-256.
 *
 * Compute-Cores post hash jobs via SPSC channel.
 * RT-Core processes them at lowest priority (RT_PRIO_HASH).
 * Results written to result channel, Compute reads back.
 *
 * Job addr is kernel-virtual (direct-mapped). Syscall handler copies
 * user data into page_alloc'd buffer before posting. RT-Core frees
 * the buffer after hashing. */

#include "core/rt.h"
#include "core/rt_poll.h"
#include "crypto/sha256.h"
#include "mm/page_alloc.h"

/* ── Job/Result wire formats ──────────────────────── */

typedef struct {
    uint64_t addr;      /* kernel-virtual address of data (page_alloc'd) */
    uint32_t len;       /* data length */
    uint32_t pages;     /* number of pages to free after hashing */
    uint64_t cookie;    /* opaque ID, returned with result */
} __attribute__((packed)) hash_job_t;

typedef struct {
    uint64_t cookie;
    uint8_t  hash[32];
} __attribute__((packed)) hash_result_t;

/* ── Channels ────────────────────────────────────── */

#define HASH_JOB_RING_SIZE    4096   /* power of 2 */
#define HASH_RESULT_RING_SIZE 4096

static uint8_t job_buf[HASH_JOB_RING_SIZE] __attribute__((aligned(64)));
static uint8_t res_buf[HASH_RESULT_RING_SIZE] __attribute__((aligned(64)));

static rt_channel_t hash_job_ring;
static rt_channel_t hash_result_ring;

/* ── RT-Core poll handler ────────────────────────── */

static int hash_poll(int max_work) {
    int done = 0;
    hash_job_t job;

    while (max_work == 0 || done < max_work) {
        int r = rt_channel_pop(&hash_job_ring, &job, sizeof(job));
        if (r < 0) break;
        if (r != (int)sizeof(job)) break;

        hash_result_t res;
        res.cookie = job.cookie;
        sha256_oneshot((const void *)(uintptr_t)job.addr, job.len, res.hash);

        /* Free the kernel buffer */
        if (job.pages > 0)
            pages_free((void *)(uintptr_t)job.addr, (int)job.pages);

        rt_channel_push(&hash_result_ring, &res, sizeof(res));
        done++;
    }
    return done;
}

/* ── Init (called from main.c) ───────────────────── */

void rt_hash_init(void) {
    rt_channel_init(&hash_job_ring, job_buf, HASH_JOB_RING_SIZE);
    rt_channel_init(&hash_result_ring, res_buf, HASH_RESULT_RING_SIZE);
    rt_poll_register(RT_PRIO_HASH, hash_poll, 8);
}

/* ── Kernel-side API (called from syscall handler) ── */

int rt_hash_submit(uint64_t kaddr, uint32_t len, uint32_t pages, uint64_t cookie) {
    hash_job_t job = { .addr = kaddr, .len = len, .pages = pages, .cookie = cookie };
    return rt_channel_push(&hash_job_ring, &job, sizeof(job));
}

int rt_hash_result(uint64_t *cookie, uint8_t hash[32]) {
    hash_result_t res;
    int r = rt_channel_pop(&hash_result_ring, &res, sizeof(res));
    if (r < 0) return -1;
    *cookie = res.cookie;
    for (int i = 0; i < 32; i++) hash[i] = res.hash[i];
    return 0;
}
