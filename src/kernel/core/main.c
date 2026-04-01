/* CosmoRT Kernel Main
 *
 * Boot: serial → page_alloc → paging → IRQ → timer → TSS → percpu → sched → proc → ELF → run
 */

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

#include "gen/init_bin.h"



/* XSAVE state — set at boot, read everywhere */
uint64_t xsave_xcr0 = XSTATE_FP | XSTATE_SSE;  /* default: x87+SSE */
uint32_t xsave_size = 512;                       /* default: FXSAVE */

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

__attribute__((cold))
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

__attribute__((cold))
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

    /* Enable SSE/SSE2 + detect XSAVE for AVX/AVX-512 */
    {
        uint64_t cr0 = arch_get_cr0();
        cr0 &= ~(1ULL << 2);  /* clear CR0.EM (no x87 emulation) */
        cr0 &= ~(1ULL << 3);  /* clear CR0.TS (allow FPU/SSE without #NM) */
        cr0 |=  (1ULL << 1);  /* set CR0.MP (monitor coprocessor) */
        arch_set_cr0(cr0);
        uint64_t cr4 = arch_get_cr4();
        cr4 |= (1 << 9) | (1 << 10); /* CR4.OSFXSR + CR4.OSXMMEXCPT */

        /* SMEP + SMAP: prevent kernel from executing/accessing user pages.
         * Check CPUID.07H:EBX — bit 7 = SMEP, bit 20 = SMAP. */
        uint32_t eax, ebx, ecx, edx;
        arch_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        if (ebx & (1U << 7)) {
            cr4 |= (1ULL << 20);  /* CR4.SMEP */
            serial_puts("sec: SMEP enabled\n");
        }
        if (ebx & (1U << 20)) {
            cr4 |= (1ULL << 21);  /* CR4.SMAP */
            serial_puts("sec: SMAP enabled\n");
        }

        /* XSAVE: detect via CPUID.01H:ECX bit 26 (OSXSAVE-capable).
         * If present: enable CR4.OSXSAVE, query supported components via
         * CPUID.0DH, configure XCR0, read save area size.
         * Like Linux: eager FPU, XSAVE on every context switch. */
        arch_cpuid(1, &eax, &ebx, &ecx, &edx);
        if (ecx & (1U << 26)) { /* XSAVE supported */
            cr4 |= (1ULL << 18);  /* CR4.OSXSAVE */
            arch_set_cr4(cr4);

            /* Query max supported XCR0 from CPUID.0DH:EAX (subleaf 0) */
            uint32_t x_eax, x_ebx, x_ecx, x_edx;
            arch_cpuid_count(0x0D, 0, &x_eax, &x_ebx, &x_ecx, &x_edx);
            uint64_t hw_xcr0 = ((uint64_t)x_edx << 32) | x_eax;

            /* Enable components we support: x87 + SSE always,
             * plus AVX and AVX-512 if hardware supports them */
            uint64_t want = XSTATE_FP | XSTATE_SSE;
            if (hw_xcr0 & XSTATE_AVX)       want |= XSTATE_AVX;
            if ((hw_xcr0 & XSTATE_AVX512_OP) &&
                (hw_xcr0 & XSTATE_AVX512_HI) &&
                (hw_xcr0 & XSTATE_AVX512_EX))
                want |= XSTATE_AVX512_OP | XSTATE_AVX512_HI | XSTATE_AVX512_EX;

            arch_xsetbv(0, want);
            xsave_xcr0 = want;

            /* Re-query with our XCR0 to get actual save area size */
            arch_cpuid_count(0x0D, 0, &x_eax, &x_ebx, &x_ecx, &x_edx);
            xsave_size = x_ebx; /* EBX = size required for enabled components */
            if (xsave_size > XSAVE_MAX_SIZE) xsave_size = XSAVE_MAX_SIZE;
            if (xsave_size < 512) xsave_size = 512;

            serial_puts("xsave: enabled (");
            if (want & XSTATE_AVX512_OP) serial_puts("AVX-512");
            else if (want & XSTATE_AVX)  serial_puts("AVX");
            else                         serial_puts("SSE");
            serial_puts(")\n");
        } else {
            /* No XSAVE — FXSAVE fallback (512 bytes, x87+SSE only) */
            xsave_xcr0 = XSTATE_FP | XSTATE_SSE;
            xsave_size = 512;
            arch_set_cr4(cr4);
            serial_puts("fpu: FXSAVE (no XSAVE)\n");
        }

        /* Ensure AC flag is clear (CLAC) — default kernel state */
        arch_clac();
    }

    /* Interrupts + Timer */
    irq_init();
    timer_init();
    rtc_init();
    extern void timer_wheel_init(void);
    timer_wheel_init();
    extern void hrtimer_init_subsystem(void);
    hrtimer_init_subsystem();
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
        extern void vfs_mount_ext2(void);
        vfs_mount_ext2();
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


    /* Futex subsystem (wait queue hash table + slab pool) */
    extern void futex_init(void);
    futex_init();

    /* Epoll/eventfd/timerfd subsystem */
    extern void epoll_init(void);
    epoll_init();

    /* SMP disabled — single-core for POSIX phase */
    extern void sched_loop(void);
    (void)sched_loop;

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
        /* Kernel NIC drivers. e1000d (Ring 3) available at /bin/e1000d
         * but requires event-based net stack for TCP (future work). */
        e1000_init();
        virtio_net_init();
        virtio_gpu_init();   /* 2D framebuffer if found */
        virtio_input_init();
    }

    /* Virtual Terminal — framebuffer + PTY + keyboard */
    vt_init(info);

    /* Input subsystem routes keyboard events to VT */
    input_init();
    /* Serial ↔ VT bridge (serial console for headless/no-keyboard) */
    serial_bridge_init();
    /* Drivers already registered themselves during probe phase above.
     * PTY stdio: use VT0 if FB+keyboard present, OR if serial is available. */
    {
        extern int fd_default_pty;
        if (info->fb_addr && input_has_keyboard())
            fd_default_pty = vt_pty_id(0);
        else
            fd_default_pty = vt_pty_id(0); /* serial console fallback */
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
            if (net_dhcp_check()) {
                dhcp_ok = 1;
                break;
            }
            if (timer_ms() - last_discover > 1000) {
                net_dhcp_send_discover();
                last_discover = timer_ms();
            }
            arch_halt(); /* sleep until next IRQ */
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
            serial_putchar('\n');
        } else {
            /* DHCP failed — use static config for QEMU user-mode networking */
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
        arch_cli_halt();
    }

    /* PTY redirect handled by vt_shell for qemu-gui, not here */

    serial_puts("--- Entering userspace ---\n\n");

    /* Enter scheduler loop (same as APs) */
    sched_loop();
}
