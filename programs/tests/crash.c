#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* CRASH.C — 意図的にNULLポインタアクセスしてPAGE FAULT復帰をテスト */



#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    volatile int *guard_page;

    api->kprintf(0x42, "%s", "Crash test: writing to guard page (0x8F000)...\n");
    guard_page = (volatile int *)0x8F000UL;
    *guard_page = 0xDEAD;

    /* ここには来ないはず */
    api->kprintf(0x42, "%s", "ERROR: should not reach here!\n");
}
