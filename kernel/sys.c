/* ======================================================================== */
/*  SYS.C — システム制御およびブザー制御                                      */
/* ======================================================================== */

#include "sys.h"
#include "io.h"
#include "pc98.h"
#include "rtc.h"
#include "os_time.h"

void sys_reboot(void)
{
    /* PC-98 ハードウェアリセット (FreeBSD実装準拠) */
    outp(SYSPORT_C_BSR, BSR_SHUT0_SET);  /* SHUT0 = 1 */
    outp(SYSPORT_C_BSR, BSR_SHUT1_SET);  /* SHUT1 = 1 */
    outp(CPU_RESET_PORT, 0x00);           /* CPUリセット */
    /* ここには来ない */
    for (;;) { __asm__ volatile("hlt"); }
}

void sys_halt(void)
{
    __asm__ volatile("hlt");
}

void buz_on(void)
{
    outp(SYSPORT_C_BSR, BSR_BUZ_ON);
}

void buz_off(void)
{
    outp(SYSPORT_C_BSR, BSR_BUZ_OFF);
}

u32 sys_mem_kb = 1024; /* 初期値(1MB) */

u32 sys_get_mem_kb(void)
{
    return sys_mem_kb;
}

os_time_t sys_time(void)
{
    RTC_Time t;
    int y;
    rtc_read(&t);
    y = t.year;
    /* PC-98のRTCは年号下2桁のみ。80以上なら1900年代、未満なら2000年代と仮定 */
    if (y < 80) y += 2000;
    else y += 1900;
    return datetime_to_epoch(y, t.month, t.day, t.hour, t.min, t.sec);
}
