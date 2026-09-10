#ifndef OS32_MEMORY_BOOT_H
#define OS32_MEMORY_BOOT_H
#include "types.h"

/* Boot-only: success permits downstream init; failure requires fail-stop.
 * Never retry or select legacy after a model bootstrap/stage attempt. */
int memory_boot_init(u32 mem_kb);
#endif
