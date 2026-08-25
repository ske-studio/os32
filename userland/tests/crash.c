#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* CRASH.C — 意図的にNULLポインタアクセスしてPAGE FAULT復帰をテスト */



#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    volatile int *guard_page;

    /* カーネルスタックガード。include/memmap.h の MEM_STACK_GUARD と
     * 一致させること (低位 0x8F000 から 0x1FB000 へ移設済み)。 */
    api->kprintf(0x42, "%s", "Crash test: writing to guard page (0x1FB000)...\n");
    guard_page = (volatile int *)0x1FB000UL;
    *guard_page = 0xDEAD;

    /* ここには来ないはず */
    api->kprintf(0x42, "%s", "ERROR: should not reach here!\n");
}
