/* Dynamic page table management */
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "boot_info.h"

void paging_init(struct boot_info *info);

void paging_map_2mb(uint64_t phys_addr);

#define PAGING_MAX_RESERVED 512
extern uint64_t paging_reserved_phys[PAGING_MAX_RESERVED];
extern int paging_reserved_count;

#endif
