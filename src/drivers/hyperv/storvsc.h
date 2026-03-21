/* CosmoRT storvsc — Hyper-V Synthetic SCSI Storage */
#ifndef STORVSC_H
#define STORVSC_H

#include <stdint.h>

int storvsc_init(void);
int storvsc_read(uint64_t block, void *buf);
int storvsc_write(uint64_t block, const void *buf);
uint64_t storvsc_capacity(void);

#endif
