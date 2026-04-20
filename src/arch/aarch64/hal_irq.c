/* aarch64 HAL - IRQ controller (stubs).
 *
 * Phase-9 placeholder. Real impl will drive GICv2/GICv3 Distributor +
 * Redistributor and install the VBAR_EL1 vector table.
 *
 * Not linked into the x86_64 build.
 */

#include "hal/hal_irq.h"
#include "kernel/panic.h"

#define AARCH64_STUB(name) \
    do { (void)0; panic("aarch64 hal_irq_" #name " not implemented"); } while (0)

void hal_irq_init(void)                          { AARCH64_STUB(init); }
void hal_irq_register(int v, irq_handler_t fn)   { (void)v; (void)fn; AARCH64_STUB(register); }
void hal_irq_unregister(int v)                   { (void)v; AARCH64_STUB(unregister); }
void hal_irq_enable(int v)                       { (void)v; AARCH64_STUB(enable); }
void hal_irq_disable(int v)                      { (void)v; AARCH64_STUB(disable); }
void hal_irq_eoi(void)                           { AARCH64_STUB(eoi); }
void hal_irq_mask_all(void)                      { AARCH64_STUB(mask_all); }
void hal_irq_send_ipi(int c, int v)              { (void)c; (void)v; AARCH64_STUB(send_ipi); }
void hal_irq_send_ipi_all(int v)                 { (void)v; AARCH64_STUB(send_ipi_all); }
void hal_irq_install_vector_table(const void *d) { (void)d; AARCH64_STUB(install_vector_table); }
