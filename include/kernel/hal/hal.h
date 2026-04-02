/* HAL — Master include for Hardware Abstraction Layer
 *
 * All kernel code above arch/ includes this instead of arch-specific headers.
 */
#ifndef HAL_H
#define HAL_H

#include "hal/hal_cpu.h"
#include "hal/hal_irq.h"
#include "hal/hal_timer.h"
#include "hal/hal_mmu.h"
#include "hal/hal_smp.h"

#endif
