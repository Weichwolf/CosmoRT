/* CosmoRT Per-CPU Data — accessed via swapgs + GS-relative addressing
 *
 * On SYSCALL entry: swapgs loads kernel GS base → percpu struct.
 * On SYSRET: swapgs restores user GS base.
 * Interrupts use LAPIC ID to index percpu_data[] directly.
 *
 * Layout: kernel_rsp at offset 0, user_rsp at offset 8 (hardcoded in ASM).
 */
#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>
#include "config.h"

struct thread;  /* forward decl */

typedef struct percpu {
    uint64_t       kernel_rsp;      /* offset 0: kernel stack for SYSCALL entry */
    uint64_t       user_rsp;        /* offset 8: saved user RSP during SYSCALL */
    struct thread *current_thread;  /* offset 16: running thread on this core */
    int            core_id;         /* offset 24 */
    int            in_kernel;       /* offset 28 */
    uint64_t       syscall_frame;   /* offset 32: saved regs pointer for clone() */
} percpu_t;

extern percpu_t percpu_data[SMP_MAX_CORES];

/* Initialize BSP per-CPU data (called once at boot) */
void percpu_init_bsp(void);

/* Initialize AP per-CPU data (called on each AP startup) */
void percpu_init_ap(int core_id);

/* Get current core's percpu (via LAPIC ID, works without swapgs) */
percpu_t *percpu_self(void);

/* Get current thread on this core */
struct thread *thread_current(void);

#endif
