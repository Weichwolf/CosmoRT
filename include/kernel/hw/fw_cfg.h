/* QEMU fw_cfg interface — read configuration data passed via -fw_cfg.
 *
 * Linux uses this for CONFIG_FW_CFG_SYSFS / dracut-fw_cfg.sh and (when no
 * boot loader has set the kernel cmdline) reads opt/cmdline at boot. We
 * use it to pass `console=ttyS0,...` style args to a UEFI-loaded CosmoRT
 * since OVMF + raw \EFI\BOOT\BOOTX64.EFI launches don't carry cmdline.
 *
 * On real hardware (no QEMU): probe returns 0, callers fall through. */
#ifndef FW_CFG_H
#define FW_CFG_H

#include <stdint.h>
#include <stddef.h>

/* Returns 1 if QEMU fw_cfg signature ("QEMU") is present, else 0.
 * Result is cached after first call. */
int fw_cfg_probe(void);

/* Look up a named file (e.g. "opt/cmdline"). Returns size in bytes if
 * found and readable into `buf` (truncates at `bufsize - 1`, NUL-terminates).
 * Returns 0 if absent. Negative on error. */
int fw_cfg_read_file(const char *name, char *buf, int bufsize);

#endif
