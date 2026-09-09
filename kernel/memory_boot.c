#include "memory_boot.h"
#include "pgalloc.h"
#include "memmap.h"

/* The loader samples one dword per 512KiB, including F00000/F80000.
 * Writable device memory can pass that test: it does NOT establish RAM in
 * [15,16)MiB. Restrict this legacy provider's admitted extent, not a blanket
 * MMIO hole policy. The unproven tail stays UNKNOWN for the device broker;
 * only a future machine provider can establish additional usable RAM.
 * Keep the raw sys_mem_kb report unchanged; sys freezes the safe tail. */
#define MEMORY_BOOT_LEGACY_END (15UL * MEM_1MB)
#define MEMORY_BOOT_WORKSPACE_PAGES 1UL

/* Small boot-owned model: never publish a caller's stack object. */
static struct physmem boot_memory;

int memory_boot_init(u32 mem_kb)
{
    struct pgalloc_layout layout;
    u32 admitted_kb, top, pages;
    admitted_kb = mem_kb;
    if (admitted_kb > MEMORY_BOOT_LEGACY_END / 1024UL)
        admitted_kb = MEMORY_BOOT_LEGACY_END / 1024UL;
    physmem_bootstrap_legacy(&boot_memory, admitted_kb);
    layout.capacity = pgalloc_metadata_bytes(&boot_memory);
    top = physmem_legacy_end(&boot_memory);
    pages = layout.capacity / PAGE_SIZE;
    /* PREINIT choice only. In particular 8MiB has no shared workspace
     * above APP_BAND_TOP; preserve its legacy allocator and exec limits. */
    if (!pages || top < pages + MEMORY_BOOT_WORKSPACE_PAGES ||
        top - pages - MEMORY_BOOT_WORKSPACE_PAGES < MEM_APP_BAND_TOP / PAGE_SIZE) {
        pgalloc_init(mem_kb);
        return 1;
    }
    layout.metadata_first = top - pages;
    layout.metadata = (void *)(layout.metadata_first * PAGE_SIZE);
    layout.workspace_end = layout.metadata_first;
    layout.workspace_first = layout.workspace_end - MEMORY_BOOT_WORKSPACE_PAGES;
    /* These calls verify real PTEs before touching metadata/workspace/hot.
     * Once attempted, failure is fatal: never reinitialize as legacy. */
    if (!sys_memory_bootstrap_model(&boot_memory, &layout, paging_verify_identity))
        return 0;
    return sys_memory_stage_online();
}
