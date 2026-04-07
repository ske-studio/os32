/* HEAP.C — ヒープ状態表示 (外部コマンド) */
#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    u32 total = api->kmalloc_total();
    u32 used  = api->kmalloc_used();
    u32 free  = api->kmalloc_free();
    api->kprintf(0xA1, "Heap: ");
    api->kprintf(0xE1, "%d", total);
    api->kprintf(0xE1, " bytes total, ");
    api->kprintf(0xE1, "%d", used);
    api->kprintf(0xE1, " used, ");
    api->kprintf(0xE1, "%d", free);
    api->kprintf(0xE1, " free\n");
}
