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
#include "procfs.h"
#include "net.h"
#include "hw.h"
#include "random.h"
#include "vt.h"
#include "fb.h"
#include "input.h"

#include "gen/init_bin.h"

/* Dynamic linker binary (embedded, registered in VFS at /lib/ld-cosmo.so) */
#ifdef HAVE_LD_COSMO
#include "gen/ld_cosmo_bin.h"
#endif

/* Userspace E1000 driver (embedded, registered in VFS at /bin/e1000d) */
#ifdef HAVE_E1000D
#include "gen/e1000d_bin.h"
#endif

/* Service manager (embedded, registered in VFS at /bin/svcmgr) */
#ifdef HAVE_SVCMGR
#include "gen/svcmgr_bin.h"
#endif

/* ISR stacks now in sched.c (per-core idle_stacks) */

/* ── EFI Relocation Fixup ─────────────────────────────
 * The EFI PE loader applies R_X86_64_RELATIVE relocations at the physical
 * load address.  After jumping to the direct map (PHYS_OFFSET + phys),
 * all absolute pointers in .data/.rodata still hold identity-map addresses.
 * Syscall handlers run with user CR3 (no identity map) → page fault.
 *
 * Fix: walk the .rela section via _DYNAMIC and add PHYS_OFFSET to every
 * R_X86_64_RELATIVE target whose current value is below PHYS_OFFSET. */

struct elf64_dyn { uint64_t tag; uint64_t val; };
struct elf64_rela { uint64_t offset; uint64_t info; int64_t addend; };
#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8

extern struct elf64_dyn _DYNAMIC[];
extern char ImageBase[];  /* link address 0 → runtime = EFI load base */

static void fixup_efi_relocations(void) {
    /* Compute EFI load base: runtime address of ImageBase (link addr 0) */
    uint64_t ldbase = (uint64_t)(uintptr_t)ImageBase;

    uint64_t rela_off = 0, rela_size = 0, rela_ent = 0;
    for (struct elf64_dyn *d = _DYNAMIC; d->tag != DT_NULL; d++) {
        if (d->tag == DT_RELA)    rela_off = d->val;
        if (d->tag == DT_RELASZ)  rela_size = d->val;
        if (d->tag == 9/*RELAENT*/) rela_ent = d->val;
    }
    if (!rela_off || !rela_size) return;
    if (!rela_ent) rela_ent = sizeof(struct elf64_rela);

    /* rela_off is link-time offset; add ldbase to get runtime phys addr,
     * then ensure_high to get direct-map kernel address */
    struct elf64_rela *rela = (struct elf64_rela *)ensure_high(rela_off + ldbase);
    int count = (int)(rela_size / rela_ent);

    int patched = 0;
    for (int i = 0; i < count; i++) {
        uint64_t type = rela[i].info & 0xFFFFFFFF;
        if (type != 8) continue; /* 8 = R_X86_64_RELATIVE */

        /* Target address = link-time offset + load base → direct map */
        uint64_t *target = (uint64_t *)ensure_high(rela[i].offset + ldbase);
        uint64_t val = *target;
        /* Value was set by _relocate to: addend + ldbase (physical address).
         * We need: addend + ldbase + PHYS_OFFSET (direct-map address). */
        if (val && val < PHYS_OFFSET) {
            *target = val + PHYS_OFFSET;
            patched++;
        }
    }
    (void)patched; /* relocation fixup complete */
}

struct boot_info *g_boot_info;

void kernel_main(struct boot_info *info) {
    /* FIRST: fix all EFI-relocated absolute pointers before any C code
     * uses global function pointers or data pointers. */
    fixup_efi_relocations();

    g_boot_info = info;

    serial_init();
    serial_puts("\n\nCosmoRT v0.1\n");

    /* CPU feature detection (ERMS, AVX2) for memops */
    memops_init();

    /* Extend direct map to cover ALL physical RAM (before page allocator) */
    paging_init(info);

    /* Page allocator — register ALL UEFI memory regions.
     * Must run AFTER paging_init so phys_to_virt works for >8GB RAM. */
    page_alloc_init(0, 0);
    page_alloc_add_uefi_regions(
        phys_to_virt(info->mmap_addr),
        info->mmap_size,
        info->mmap_desc_size);

    /* Enable SSE/SSE2 on BSP (user code uses SSE for string ops) */
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2);  /* clear CR0.EM (no x87 emulation) */
        cr0 |=  (1ULL << 1);  /* set CR0.MP (monitor coprocessor) */
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 9) | (1 << 10); /* CR4.OSFXSR + CR4.OSXMMEXCPT */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    }

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

    /* procfs — virtual /proc (dmesg, meminfo, cpuinfo) */
    procfs_init();

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

    /* Epoll/eventfd/timerfd subsystem */
    extern void epoll_init(void);
    epoll_init();

    /* SMP — APs enter scheduler loop */
    extern void sched_loop(void);
    smp_start_all(sched_loop);

    /* Drivers — probe and self-register with subsystems */
    extern int hyperv_detect(void);
    extern void hyperv_init(void);
    extern void hyperv_synic_init(void);
    extern int vmbus_init(void);
    extern int storvsc_init(void);
    extern int netvsc_init(void);
    extern int hyperv_fb_init(void);
    extern int hv_kbd_init(void);
    extern int hv_mouse_init(void);
    extern int hv_utils_init(void);
    extern int e1000_init(void);
    extern int virtio_net_init(void);
    extern int virtio_gpu_init(void);
    extern int virtio_input_init(void);

    if (hyperv_detect()) {
        serial_puts("Hyper-V detected\n");
        hyperv_init();
        hyperv_synic_init();
        if (vmbus_init() == 0) {
            storvsc_init();     /* block device */
            netvsc_init();      /* network adapter */
            hyperv_fb_init();   /* framebuffer */
            hv_kbd_init();      /* keyboard */
            hv_mouse_init();    /* mouse */
            hv_utils_init();    /* heartbeat, shutdown, timesync */
        }
    } else {
        e1000_init();        /* registers with net stack if found */
        virtio_net_init();   /* alternative NIC, registers if found */
        virtio_gpu_init();   /* 2D framebuffer if found */
        virtio_input_init();
    }

    /* Virtual Terminal — framebuffer + PTY + keyboard */
    vt_init(info);

    /* Input subsystem routes keyboard events to VT */
    input_init();
    /* Drivers already registered themselves during probe phase above.
     * PTY stdio when both FB and keyboard present. */
    {
        extern int fd_default_pty;
        if (info->fb_addr && input_has_keyboard())
            fd_default_pty = vt_pty_id(0);
    }

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

    /* Welcome message on VT0 (through normal rendering pipeline) */
    if (fb_available()) {
        /* Helper: write string to VT0 via vt_process_byte */
        #define VT_PUTS(s) do { const char *_p = (s); while (*_p) vt_process_byte(0, (uint8_t)*_p++); } while(0)
        /* Helper: write decimal int to VT0 */
        #define VT_INT(v) do { \
            int _v = (v); char _t[12]; int _j = 0; \
            if (_v == 0) { vt_process_byte(0, '0'); } else { \
            while (_v > 0) { _t[_j++] = (char)('0' + _v % 10); _v /= 10; } \
            while (_j--) vt_process_byte(0, (uint8_t)_t[_j]); } \
        } while(0)

        VT_PUTS("\033[1;36mCosmoRT\033[0m v0.1\n\n");
        VT_PUTS("  Cores: "); VT_INT(smp_num_cores()); VT_PUTS("\n");
        VT_PUTS("  RAM:   "); VT_INT(page_alloc_total() * 4 / 1024); VT_PUTS(" MB\n");
        if (net_my_ip[0]) {
            VT_PUTS("  Net:   ");
            for (int i = 0; i < 4; i++) {
                VT_INT(net_my_ip[i]);
                if (i < 3) vt_process_byte(0, '.');
            }
            VT_PUTS("\n");
        }
        VT_PUTS("\n");
        vt_render_dirty(0);

        #undef VT_PUTS
        #undef VT_INT
    }

    serial_puts("\n--- Loading init ---\n");

    int pid = proc_create_elf(init_bin, init_bin_size);
    if (pid < 0) {
        serial_puts("FATAL: failed to load init\n");
        __asm__ volatile("cli; hlt");
    }

    /* PTY redirect handled by vt_shell for qemu-gui, not here */

    serial_puts("--- Entering userspace ---\n\n");

    /* Enter scheduler loop (same as APs) */
    sched_loop();
}
