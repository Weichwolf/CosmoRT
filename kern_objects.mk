# Kernel object files — shared between root Makefile and test/Makefile
# This file uses $(BUILD), $(SRC), $(ARCH_DIR) from the including Makefile.

# ── ASM objects ──
KERN_ASM = $(BUILD)/kernel/entry.o \
           $(BUILD)/kernel/irq_asm.o \
           $(BUILD)/kernel/context.o \
           $(BUILD)/kernel/syscall_entry.o \
           $(BUILD)/kernel/memops.o

# ── Kernel C objects by subdirectory ──
KERN_CORE = $(BUILD)/kernel/core/main.o \
            $(BUILD)/kernel/core/irq.o \
            $(BUILD)/kernel/core/sched.o \
            $(BUILD)/kernel/core/timer.o \
            $(BUILD)/kernel/core/smp.o \
            $(BUILD)/kernel/core/tss.o \
            $(BUILD)/kernel/core/percpu.o \
            $(BUILD)/kernel/core/timer_wheel.o \
            $(BUILD)/kernel/core/event_queue.o \
            $(BUILD)/kernel/core/rbtree.o \
            $(BUILD)/kernel/core/hrtimer.o

KERN_MM   = $(BUILD)/kernel/mm/page_alloc.o \
            $(BUILD)/kernel/mm/paging.o \
            $(BUILD)/kernel/mm/vma.o \
            $(BUILD)/kernel/mm/slab.o \
            $(BUILD)/kernel/mm/random.o \
            $(BUILD)/kernel/mm/page_cache.o

KERN_PROC = $(BUILD)/kernel/proc/process.o \
            $(BUILD)/kernel/proc/process_lazy.o \
            $(BUILD)/kernel/proc/process_fork.o \
            $(BUILD)/kernel/proc/process_exec.o \
            $(BUILD)/kernel/proc/process_wait.o \
            $(BUILD)/kernel/proc/elf.o \
            $(BUILD)/kernel/proc/signal.o \
            $(BUILD)/kernel/proc/signal_frame.o \
            $(BUILD)/kernel/proc/signal_handler.o

KERN_SYS  = $(BUILD)/kernel/sys/dispatch.o \
             $(BUILD)/kernel/sys/sys_file.o \
             $(BUILD)/kernel/sys/sys_fs.o \
             $(BUILD)/kernel/sys/sys_mem.o \
             $(BUILD)/kernel/sys/sys_proc.o \
             $(BUILD)/kernel/sys/sys_sched.o \
             $(BUILD)/kernel/sys/sys_time.o \
             $(BUILD)/kernel/sys/sys_ipc.o \
             $(BUILD)/kernel/sys/sys_net.o \
             $(BUILD)/kernel/sys/sys_event.o \
             $(BUILD)/kernel/sys/sys_id.o \
             $(BUILD)/kernel/sys/stubs.o \
             $(BUILD)/kernel/sys/sys_cosmo.o

KERN_IPC  = $(BUILD)/kernel/ipc/ipc.o \
            $(BUILD)/kernel/ipc/futex.o \
            $(BUILD)/kernel/ipc/net_port.o \
            $(BUILD)/kernel/ipc/sysv_ipc.o

KERN_FS   = $(BUILD)/kernel/fs/vfs.o \
            $(BUILD)/kernel/fs/vfs_mount.o \
            $(BUILD)/kernel/fs/vfs_lookup.o \
            $(BUILD)/kernel/fs/vfs_rw.o \
            $(BUILD)/kernel/fs/vfs_dirops.o \
            $(BUILD)/kernel/fs/vfs_ioctls.o \
            $(BUILD)/kernel/fs/vfs_symlink.o \
            $(BUILD)/kernel/fs/ext2.o \
            $(BUILD)/kernel/fs/ext2_vfs.o \
            $(BUILD)/kernel/fs/tmpfs.o \
            $(BUILD)/kernel/fs/devfs.o \
            $(BUILD)/kernel/fs/bcache.o \
            $(BUILD)/kernel/fs/procfs.o

KERN_NET  = $(BUILD)/kernel/net/net.o \
            $(BUILD)/kernel/net/dispatch.o \
            $(BUILD)/kernel/net/arp.o \
            $(BUILD)/kernel/net/ip.o \
            $(BUILD)/kernel/net/tcp.o \
            $(BUILD)/kernel/net/udp.o \
            $(BUILD)/kernel/net/dns.o \
            $(BUILD)/kernel/net/dhcp.o \
            $(BUILD)/kernel/net/socket.o \
            $(BUILD)/kernel/net/unix_socket.o

KERN_EVENT = $(BUILD)/kernel/event/epoll.o \
             $(BUILD)/kernel/event/eventfd.o \
             $(BUILD)/kernel/event/timerfd.o \
             $(BUILD)/kernel/event/signalfd.o \
             $(BUILD)/kernel/event/inotify.o

KERN_VT   = $(BUILD)/kernel/vt/vt.o \
            $(BUILD)/kernel/vt/pty.o \
            $(BUILD)/kernel/vt/fb.o \
            $(BUILD)/kernel/vt/input.o

KERN_HAL  = $(BUILD)/arch/x86_64/cpu/hal_cpu.o \
            $(BUILD)/arch/x86_64/timer/hal_timer.o

KERN_HW   = $(BUILD)/kernel/hw/cosmort.o \
            $(BUILD)/kernel/hw/serial.o \
            $(BUILD)/kernel/hw/serial_bridge.o \
            $(BUILD)/kernel/hw/kexec.o \
            $(BUILD)/arch/x86_64/hw/hyperv.o \
            $(BUILD)/arch/x86_64/hw/qemu.o \
            $(BUILD)/arch/x86_64/sha256.o

KERN_DRV  = $(BUILD)/drivers/virtio/virtio.o \
            $(BUILD)/drivers/virtio/virtio_net.o \
            $(BUILD)/drivers/virtio/virtio_blk.o \
            $(BUILD)/drivers/virtio/virtio_gpu.o \
            $(BUILD)/drivers/virtio/virtio_input.o \
            $(BUILD)/drivers/pci/e1000.o \
            $(BUILD)/drivers/hyperv/vmbus.o \
            $(BUILD)/drivers/hyperv/storvsc.o \
            $(BUILD)/drivers/hyperv/netvsc.o \
            $(BUILD)/drivers/hyperv/hyperv_fb.o \
            $(BUILD)/drivers/hyperv/hv_kbd.o \
            $(BUILD)/drivers/hyperv/hv_mouse.o \
            $(BUILD)/drivers/hyperv/hv_utils.o

KERN_OBJ = $(KERN_ASM) $(KERN_CORE) $(KERN_MM) $(KERN_PROC) \
           $(KERN_SYS) $(KERN_IPC) $(KERN_FS) $(KERN_NET) \
           $(KERN_EVENT) $(KERN_VT) $(KERN_HAL) $(KERN_HW) $(KERN_DRV)

# All kernel objects except main.o (for test builds that substitute their own)
KERN_OBJ_NO_MAIN = $(filter-out $(BUILD)/kernel/core/main.o,$(KERN_OBJ))
