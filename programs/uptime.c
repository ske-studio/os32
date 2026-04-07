/* UPTIME.C — 稼働時間表示 (外部コマンド) */
#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    u32 ticks = api->get_tick();
    u32 secs = ticks / 100;
    u32 mins = secs / 60;
    api->kprintf(0xE1, "up %u min %u sec\n", mins, secs % 60);
}
