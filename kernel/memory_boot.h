#ifndef OS32_MEMORY_BOOT_H
#define OS32_MEMORY_BOOT_H
#include "types.h"

/* Boot-only: success permits downstream init; failure requires fail-stop.
 * Never retry or select legacy after a model bootstrap/stage attempt. */
int memory_boot_init(u32 mem_kb);

/* Physical RAM extent in KiB (top-of-RAM address / 1024), folding the old
 * loader's 512KiB probe report together with RAM at/above MEM_HIGH_RAM_BASE.
 * Call ONCE in kernel_main BEFORE paging_init, with paging off and IF=0: it
 * write-probes one dword per megabyte to confirm the BIOS work area and to
 * detect 24-bit address wrap. Returns mem_kb unchanged when no high RAM is
 * reported or confirmed. There is no artificial ceiling here; the reported
 * extent is only bounded by the top-of-4GiB ROM/MMIO band. */
u32 memory_boot_detect(u32 mem_kb);
#endif
