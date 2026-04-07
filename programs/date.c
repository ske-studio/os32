/* DATE.C — 日時表示 (外部コマンド) */
#include "os32api.h"

static const char *wday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

void main(int argc, char **argv, KernelAPI *api)
{
    RTC_Time_Ext t;
    const char *w;
    api->rtc_read(&t);
    w = (t.wday < 7) ? wday_names[t.wday] : "???";
    api->kprintf(0xE1, "20%u%u-%u%u-%u%u %u%u:%u%u:%u%u (%s)\n",
        (u32)(t.year / 10), (u32)(t.year % 10),
        (u32)(t.month / 10), (u32)(t.month % 10),
        (u32)(t.day / 10), (u32)(t.day % 10),
        (u32)(t.hour / 10), (u32)(t.hour % 10),
        (u32)(t.min / 10), (u32)(t.min % 10),
        (u32)(t.sec / 10), (u32)(t.sec % 10),
        w);
}
