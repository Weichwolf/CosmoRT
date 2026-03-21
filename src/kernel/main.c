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
#include "vfs.h"
#include "net.h"
#include "hw.h"
#include "random.h"

#include "init_bin.h"

/* Dynamic linker binary (embedded, registered in VFS at /lib/ld-cosmo.so) */
#ifdef HAVE_LD_COSMO
#include "ld_cosmo_bin.h"
#endif

/* Userspace E1000 driver (embedded, registered in VFS at /bin/e1000d) */
#ifdef HAVE_E1000D
#include "e1000d_bin.h"
#endif

/* Service manager (embedded, registered in VFS at /bin/svcmgr) */
#ifdef HAVE_SVCMGR
#include "svcmgr_bin.h"
#endif

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
    random_init(info);

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

    /* Hardware primitives (MMIO, DMA, IRQ, PCI, FW) */
    extern void hw_init(void);
    hw_init();

    /* Net port (userspace NIC driver bridge) */
    extern void net_port_init(void);
    net_port_init();

    /* Block driver (probe before VFS so we know if disk is present) */
    extern int virtio_blk_init(void);
    extern uint64_t blk_capacity(void);
    extern void bcache_init(void);
    int has_disk = (virtio_blk_init() == 0 && blk_capacity() > 0);
    if (has_disk) bcache_init();

    /* VFS (ramfs always available for /dev/shm; CosmoFS for / if disk present) */
    vfs_init();
    vfs_create("/dev", VFS_DIR);
    vfs_create("/dev/shm", VFS_DIR);

    if (has_disk) {
        extern void vfs_mount_cosmofs(void);
        vfs_mount_cosmofs();
    }

    /* ramfs directories (fallback for no-disk boots) */
    if (!has_disk) {
        vfs_create("/home", VFS_DIR);
        vfs_create("/home/Documents", VFS_DIR);
        vfs_create("/home/Downloads", VFS_DIR);
        vfs_create("/home/Pictures", VFS_DIR);
        vfs_create("/home/Music", VFS_DIR);
        vfs_create("/home/Videos", VFS_DIR);
        vfs_create("/home/Projects", VFS_DIR);
        vfs_create("/tmp", VFS_DIR);
        vfs_create("/bin", VFS_DIR);
        vfs_create("/etc", VFS_DIR);
    }

    /* Dynamic linker: register at /lib/ld-cosmo.so */
#ifdef HAVE_LD_COSMO
    vfs_create("/lib", VFS_DIR);
    vfs_add_file("/lib/ld-cosmo.so", ld_cosmo_bin, ld_cosmo_bin_size);
    serial_puts("vfs: /lib/ld-cosmo.so registered\n");
#endif

    /* Userspace driver binaries */
#ifdef HAVE_E1000D
    vfs_add_file("/bin/e1000d", e1000d_bin, e1000d_bin_size);
    serial_puts("vfs: /bin/e1000d registered\n");
#endif
#ifdef HAVE_SVCMGR
    vfs_add_file("/bin/svcmgr", svcmgr_bin, svcmgr_bin_size);
    serial_puts("vfs: /bin/svcmgr registered\n");
#endif

    /* Futex subsystem (wait queue hash table + slab pool) */
    extern void futex_init(void);
    futex_init();

    /* SMP — APs enter scheduler loop */
    extern void sched_loop(void);
    smp_start_all(sched_loop);

    /* Drivers — probe and self-register with subsystems */
    extern int e1000_init(void);
    extern int virtio_net_init(void);
    extern int virtio_gpu_init(void);
    extern int virtio_input_init(void);

    e1000_init();        /* registers with net stack if found */
    virtio_net_init();   /* alternative NIC, registers if found */
    virtio_gpu_init();   /* 2D framebuffer if found */
    virtio_input_init(); /* keyboard/mouse if found */

    /* Network — uses whatever NIC driver registered */
    if (net_init() == 0) {
        serial_puts("net: DHCP...");
        net_dhcp_send_discover();

        /* Blocking DHCP wait (up to 5 seconds, retry every 1s) */
        uint64_t dhcp_deadline = timer_ms() + 5000;
        uint64_t last_discover = timer_ms();
        int dhcp_ok = 0;
        while (timer_ms() < dhcp_deadline) {
            net_poll();
            if (net_dhcp_check()) {
                dhcp_ok = 1;
                break;
            }
            if (timer_ms() - last_discover > 1000) {
                net_dhcp_send_discover();
                last_discover = timer_ms();
            }
            __asm__ volatile("sti; hlt"); /* sleep until next IRQ */
        }

        if (dhcp_ok) {
            serial_puts("ok ");
            /* Print IP */
            for (int i = 0; i < 4; i++) {
                uint8_t b = net_my_ip[i];
                if (b >= 100) serial_putchar('0' + b / 100);
                if (b >= 10) serial_putchar('0' + (b / 10) % 10);
                serial_putchar('0' + b % 10);
                if (i < 3) serial_putchar('.');
            }
            serial_puts(" gw ");
            for (int i = 0; i < 4; i++) {
                uint8_t b = net_gw_ip[i];
                if (b >= 100) serial_putchar('0' + b / 100);
                if (b >= 10) serial_putchar('0' + (b / 10) % 10);
                serial_putchar('0' + b % 10);
                if (i < 3) serial_putchar('.');
            }
            if (net_dns_ip[0]) {
                serial_puts(" dns ");
                for (int i = 0; i < 4; i++) {
                    uint8_t b = net_dns_ip[i];
                    if (b >= 100) serial_putchar('0' + b / 100);
                    if (b >= 10) serial_putchar('0' + (b / 10) % 10);
                    serial_putchar('0' + b % 10);
                    if (i < 3) serial_putchar('.');
                }
            }
            /* mDNS hostname: cosmo-XXXX where XXXX = last 4 hex of MAC */
            {
                char hn[] = "cosmo-0000";
                const char *hex = "0123456789abcdef";
                hn[6] = hex[net_my_mac[4] >> 4];
                hn[7] = hex[net_my_mac[4] & 0xF];
                hn[8] = hex[net_my_mac[5] >> 4];
                hn[9] = hex[net_my_mac[5] & 0xF];
                net_set_hostname(hn);
                serial_puts("\nnet: hostname ");
                serial_puts(hn);
                serial_puts(".local\n");
            }
        } else {
            serial_puts("timeout (no network)\n");
        }
    } else {
        serial_puts("net: no NIC (ok for basic boot)\n");
    }

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
