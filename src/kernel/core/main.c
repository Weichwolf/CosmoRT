/* CosmoRT Kernel Main */

#include "boot_info.h"
#include "hw/serial.h"
#include "mm/page_alloc.h"
#include "mm/paging.h"
#include "core/irq.h"
#include "core/timer.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "ipc/ipc.h"
#include "core/smp.h"
#include "memops.h"
#include "config.h"
#include "mm/vma.h"
#include "fs/vfs.h"
#include "fs/procfs.h"
#include "net/net.h"
#include "cosmort.h"
#include "random.h"
#include "arch/arch.h"
#include "vt/vt.h"
#include "vt/fb.h"
#include "vt/input.h"
#include "event/epoll.h"

#include "gen/init_bin.h"

struct elf64_dyn { uint64_t tag; uint64_t val; };
struct elf64_rela { uint64_t offset; uint64_t info; int64_t addend; };
#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8

extern struct elf64_dyn _DYNAMIC[];
extern char ImageBase[];

__attribute__((cold))
static void fixup_efi_relocations(void) {
    uint64_t ldbase = (uint64_t)(uintptr_t)ImageBase;

    uint64_t rela_off = 0, rela_size = 0, rela_ent = 0;
    for (struct elf64_dyn *d = _DYNAMIC; d->tag != DT_NULL; d++) {
        if (d->tag == DT_RELA)    rela_off = d->val;
        if (d->tag == DT_RELASZ)  rela_size = d->val;
        if (d->tag == 9) rela_ent = d->val;
    }
    if (!rela_off || !rela_size) return;
    if (!rela_ent) rela_ent = sizeof(struct elf64_rela);

    struct elf64_rela *rela = (struct elf64_rela *)ensure_high(rela_off + ldbase);
    int count = (int)(rela_size / rela_ent);

    int patched = 0;
    for (int i = 0; i < count; i++) {
        uint64_t type = rela[i].info & 0xFFFFFFFF;
        if (type != 8) continue;

        uint64_t *target = (uint64_t *)ensure_high(rela[i].offset + ldbase);
        uint64_t val = *target;
        if (val && val < PHYS_OFFSET) {
            *target = val + PHYS_OFFSET;
            patched++;
        }
    }
    (void)patched;
}

struct boot_info *g_boot_info;

__attribute__((cold))
void kernel_main(struct boot_info *info) {
    fixup_efi_relocations();

    g_boot_info = info;

    serial_init();
    serial_puts("\n\nCosmoRT v0.1\n");

    memops_init();

    paging_init(info);

    page_alloc_init(0, 0);
    page_alloc_add_uefi_regions(
        phys_to_virt(info->mmap_addr),
        info->mmap_size,
        info->mmap_desc_size);

    {
        uint64_t cr0 = arch_get_cr0();
        cr0 &= ~(1ULL << 2);
        cr0 &= ~(1ULL << 3);
        cr0 |=  (1ULL << 1);
        arch_set_cr0(cr0);
        uint64_t cr4 = arch_get_cr4();
        cr4 |= (1 << 9) | (1 << 10);

        uint32_t eax, ebx, ecx, edx;
        arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        if (ebx & (1U << 7)) {
            cr4 |= (1ULL << 20);
            serial_puts("sec: SMEP enabled\n");
        }
        if (ebx & (1U << 20)) {
            cr4 |= (1ULL << 21);
            serial_puts("sec: SMAP enabled\n");
        }

        arch_set_cr4(cr4);

        arch_clac();
    }

    irq_init();
    timer_init();
    rtc_init();
    extern void timer_wheel_init(void);
    timer_wheel_init();
    random_init(info);

    extern void tss_init(void);
    extern void syscall_init(void);
    tss_init();
    syscall_init();

    percpu_init_bsp();

    extern void sched_init(void);
    sched_init();

    proc_init();
    vma_init();
    ipc_init();

    extern void hw_init(void);
    hw_init();

    extern void net_port_init(void);
    net_port_init();

    extern int virtio_blk_init(void);
    extern uint64_t blk_capacity(void);
    extern void bcache_init(void);
    int has_disk = (virtio_blk_init() == 0 && blk_capacity() > 0);
    if (has_disk) bcache_init();

    vfs_init();
    vfs_create("/dev", VFS_DIR);
    vfs_create("/dev/shm", VFS_DIR);

    if (has_disk) {
        extern void vfs_mount_ext2(void);
        vfs_mount_ext2();
    }

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

    procfs_init();

    extern void futex_init(void);
    futex_init();

    epoll_init();

    extern void sched_loop(void);
    smp_start_all(sched_loop);

    {
        int ncores = smp_num_cores();
        extern void sched_isolate_core(int core_id);
        for (int c = 2; c < ncores; c++)
            sched_isolate_core(c);
        if (ncores > 2) {
            serial_puts("sched: isolated cores 2..");
            char t[4]; int ti = 0, v = ncores - 1;
            do { t[ti++] = '0' + v % 10; v /= 10; } while (v);
            while (ti--) serial_putchar(t[ti]);
            serial_putchar('\n');
        }
    }

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
            storvsc_init();
            netvsc_init();
            hyperv_fb_init();
            hv_kbd_init();
            hv_mouse_init();
            hv_utils_init();
        }
    } else {
        e1000_init();
        virtio_net_init();
        virtio_gpu_init();
        virtio_input_init();
    }

    vt_init(info);

    input_init();
    serial_bridge_init();
    {
        extern int fd_default_pty;
        if (info->fb_addr && input_has_keyboard())
            fd_default_pty = vt_pty_id(0);
        else
            fd_default_pty = vt_pty_id(0);
    }

    if (net_init() == 0) {
        serial_puts("net: DHCP...");
        net_dhcp_send_discover();

        uint64_t dhcp_deadline = timer_ms() + 5000;
        uint64_t last_discover = timer_ms();
        int dhcp_ok = 0;
        while (timer_ms() < dhcp_deadline) {
            if (net_dhcp_check()) {
                dhcp_ok = 1;
                break;
            }
            if (timer_ms() - last_discover > 1000) {
                net_dhcp_send_discover();
                last_discover = timer_ms();
            }
            lapic_delay_ms(10);
        }

        if (dhcp_ok) {
            serial_puts("ok ");
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
            serial_putchar('\n');
        } else {
            serial_puts("timeout, fallback static ");
            net_my_ip[0] = 10; net_my_ip[1] = 0; net_my_ip[2] = 2; net_my_ip[3] = 15;
            net_gw_ip[0] = 10; net_gw_ip[1] = 0; net_gw_ip[2] = 2; net_gw_ip[3] = 2;
            net_dns_ip[0] = 10; net_dns_ip[1] = 0; net_dns_ip[2] = 2; net_dns_ip[3] = 3;
            dhcp_ok = 1;
            serial_puts("10.0.2.15\n");
        }
    } else {
        serial_puts("net: no NIC (ok for basic boot)\n");
    }

    if (fb_available()) {
        #define VT_PUTS(s) do { const char *_p = (s); while (*_p) vt_process_byte(0, (uint8_t)*_p++); } while(0)
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
        arch_cli_halt();
    }

    serial_puts("--- Entering userspace ---\n\n");

    sched_loop();
}
