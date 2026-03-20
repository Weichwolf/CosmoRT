/* CosmoRT Kernel Main
 *
 * Boot: serial → page_alloc → paging → IRQ → timer → TSS → percpu → sched → proc → ELF → run
 */

#include "boot_info.h"
#include "serial.h"
#include "page_alloc.h"
#include "paging.h"
#include "irq.h"
#include "timer.h"
#include "process.h"
#include "percpu.h"
#include "ipc.h"
#include "smp.h"
#include "memops.h"
#include "config.h"
#include "vma.h"

#include "init_bin.h"

/* ISR stacks now in sched.c (per-core idle_stacks) */

uint64_t g_heap_size;
struct boot_info *g_boot_info;

static void find_heap(struct boot_info *info, uint8_t **base, uint64_t *size) {
    uint8_t *mmap = (uint8_t *)phys_to_virt(info->mmap_addr);
    uint64_t desc_size = info->mmap_desc_size;
    uint64_t count = info->mmap_size / desc_size;
    *base = 0; *size = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = *(uint32_t *)(mmap + i * desc_size);
        uint64_t phys = *(uint64_t *)(mmap + i * desc_size + 8);
        uint64_t pages = *(uint64_t *)(mmap + i * desc_size + 24);
        uint64_t region_size = pages * 4096;
        if (type == 7 && region_size > *size && phys >= 0x100000) {
            *base = (uint8_t *)phys_to_virt(phys);
            *size = region_size;
        }
    }
    if (*size > 128 * 1024 * 1024) *size = 128 * 1024 * 1024;
}

static void serial_uint(uint64_t v) {
    char t[20]; int i = 0;
    do { t[i++] = '0' + v % 10; v /= 10; } while (v);
    while (i--) serial_putchar(t[i]);
}

void kernel_main(struct boot_info *info) {
    g_boot_info = info;

    serial_init();
    serial_puts("\n\nCosmoRT v0.1\n");

    /* CPU feature detection (ERMS, AVX2) for memops */
    memops_init();

    /* Page allocator */
    uint8_t *heap_base;
    find_heap(info, &heap_base, &g_heap_size);
    page_alloc_init(heap_base, (size_t)g_heap_size);
    serial_puts("Heap: "); serial_uint(g_heap_size / (1024*1024)); serial_puts(" MB\n");

    /* Page tables */
    paging_init(info);

    /* Interrupts + Timer */
    irq_init();
    timer_init();

    /* TSS + SYSCALL */
    extern void tss_init(void);
    extern void syscall_init(void);
    tss_init();
    syscall_init();

    /* Per-CPU data + swapgs setup */
    percpu_init_bsp();

    /* Scheduler */
    extern void sched_init(void);
    sched_init();

    /* Process subsystem (slab pools) */
    proc_init();
    vma_init();
    ipc_init();

    /* Futex subsystem (wait queue hash table + slab pool) */
    extern void futex_init(void);
    futex_init();

    /* SMP — APs enter scheduler loop */
    extern void sched_loop(void);
    smp_start_all(sched_loop);

    serial_puts("\n--- Loading init ---\n");

    int pid = proc_create_elf(init_bin, init_bin_size);
    if (pid < 0) {
        serial_puts("FATAL: failed to load init\n");
        __asm__ volatile("cli; hlt");
    }

    serial_puts("--- Entering userspace ---\n\n");

    /* Enter scheduler loop (same as APs) */
    sched_loop();
}
