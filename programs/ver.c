/* VER.C — OSバージョン表示 (外部コマンド) */
#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    api->kprintf(0xA1, "PC-9801 OS32 v0.6 (Phase 6)\n");
    api->kprintf(0xC1, "  CPU: Intel 386+ (Protected Mode + Paging)\n");
    api->kprintf(0xC1, "  PIC: 8259A x2 (remapped to INT 20h+)\n");
    api->kprintf(0xC1, "  PIT: 8254 @ 100Hz\n");
    api->kprintf(0xC1, "  KBD: uPD8251A (IRQ1)\n");
    api->kprintf(0xC1, "  SER: uPD8251A RS-232C (IRQ4)\n");
    api->kprintf(0xC1, "  SND: YM2203 (OPN) FM3+SSG3\n");
    api->kprintf(0xC1, "  GFX: 640x400x16 CPU direct\n");
    api->kprintf(0xE1, "  API: v%u\n", api->version);
}
