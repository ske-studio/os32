/* TICK.C — タイマティック表示 (外部コマンド) */
#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    u32 ticks = api->get_tick();
    u32 secs = ticks / 100;
    api->kprintf(0xE1, "Timer ticks: ");
    api->kprintf(0xE1, "%d", ticks);
    api->kprintf(0xE1, " (%u sec)\n", secs);
}
