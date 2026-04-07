/* MEM.C — メモリマップ表示 (外部コマンド) */
#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    api->kprintf(0xA1, "Memory map:\n");
    api->kprintf(0xE1, "  Conventional : 0x00000 - 0x9FFFF (640KB)\n");
    api->kprintf(0xE1, "  Text VRAM    : 0xA0000 - 0xA3FFF\n");
    api->kprintf(0xE1, "  Kernel       : 0x09000 -\n");
    api->kprintf(0xE1, "  Heap         : 0x40000 - 0x6FFFF (192KB)\n");
    api->kprintf(0xE1, "  Stack        : 0x90000 - 0x9FFFC (64KB)\n");
    api->kprintf(0xE1, "  Guard page   : 0x8F000 (stack overflow trap)\n");
    if (api->paging_enabled()) {
        api->kprintf(0xC1, "  Paging       : ENABLED (identity map, 16MB)\n");
    } else {
        api->kprintf(0x41, "  Paging       : DISABLED\n");
    }
}
