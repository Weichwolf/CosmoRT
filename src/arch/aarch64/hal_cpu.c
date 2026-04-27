/* aarch64 HAL - CPU primitives.
 *
 * Interface stubs for Phase 9. Each call panics today so a linker error
 * in a would-be aarch64 build fails loud. When the port starts, these
 * get real implementations backed by WFE/WFI/DMB/MSR-TPIDR_EL0/MRS.
 *
 * Not linked into the x86_64 build.
 */

#include "hal/hal_cpu.h"
#include "kernel/panic.h"
#include <stddef.h>

#define AARCH64_STUB(name) \
    do { (void)0; panic("aarch64 hal_cpu_" #name " not implemented"); } while (0)

int hal_cpu_id(void)                                 { AARCH64_STUB(id); return 0; }
int hal_cpu_count(void)                              { AARCH64_STUB(count); return 0; }
void hal_cpu_halt(void)                              { AARCH64_STUB(halt); }
void hal_cpu_halt_noirq(void)                        { AARCH64_STUB(halt_noirq); }
void hal_cpu_relax(void)                             { AARCH64_STUB(relax); }
void hal_cpu_cli(void)                               { AARCH64_STUB(cli); }
void hal_cpu_sti(void)                               { AARCH64_STUB(sti); }
uint64_t hal_cpu_save_irq(void)                      { AARCH64_STUB(save_irq); return 0; }
void hal_cpu_restore_irq(uint64_t f)                 { (void)f; AARCH64_STUB(restore_irq); }
void hal_cpu_mfence(void)                            { AARCH64_STUB(mfence); }
void hal_cpu_wmb(void)                               { AARCH64_STUB(wmb); }
void hal_cpu_rmb(void)                               { AARCH64_STUB(rmb); }
void hal_cpu_store_release(volatile uint32_t *p, uint32_t v) { (void)p; (void)v; AARCH64_STUB(store_release); }
uint32_t hal_cpu_load_acquire(volatile uint32_t *p)  { (void)p; AARCH64_STUB(load_acquire); return 0; }
uint64_t hal_cpu_timestamp(void)                     { AARCH64_STUB(timestamp); return 0; }
uint64_t hal_cpu_stack_ptr(void)                     { AARCH64_STUB(stack_ptr); return 0; }
void hal_cpu_set_tls(uint64_t b)                     { (void)b; AARCH64_STUB(set_tls); }
uint64_t hal_cpu_get_tls(void)                       { AARCH64_STUB(get_tls); return 0; }
void hal_cpu_set_user_gs(uint64_t b)                 { (void)b; AARCH64_STUB(set_user_gs); }
uint64_t hal_cpu_get_user_gs(void)                   { AARCH64_STUB(get_user_gs); return 0; }
int hal_cpu_canonical_user_addr(uint64_t addr)       { (void)addr; return 1; }
const char *hal_cpu_arch_name(void)                  { return "aarch64"; }
void hal_cpu_fpu_save(void *a)                       { (void)a; AARCH64_STUB(fpu_save); }
void hal_cpu_fpu_restore(const void *a)              { (void)a; AARCH64_STUB(fpu_restore); }
void hal_cpu_set_percpu_base(uint64_t b)             { (void)b; AARCH64_STUB(set_percpu_base); }
void hal_cpu_set_percpu_active(uint64_t b)           { (void)b; AARCH64_STUB(set_percpu_active); }
int hal_cpu_hwrand(uint64_t *o)                      { (void)o; AARCH64_STUB(hwrand); return 0; }
void hal_cpu_fpu_boot_init(void)                     { AARCH64_STUB(fpu_boot_init); }
void hal_cpu_user_access_begin(void)                 { AARCH64_STUB(user_access_begin); }
void hal_cpu_user_access_end(void)                   { AARCH64_STUB(user_access_end); }
void hal_cpu_shutdown(void)                          { AARCH64_STUB(shutdown); }
void hal_cpu_reset(void)                             { AARCH64_STUB(reset); }
void hal_io_outb(uint16_t p, uint8_t v)              { (void)p; (void)v; AARCH64_STUB(io_outb); }
uint8_t hal_io_inb(uint16_t p)                       { (void)p; AARCH64_STUB(io_inb); return 0; }
void hal_io_outl(uint16_t p, uint32_t v)             { (void)p; (void)v; AARCH64_STUB(io_outl); }
uint32_t hal_io_inl(uint16_t p)                      { (void)p; AARCH64_STUB(io_inl); return 0; }
