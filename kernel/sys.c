/* ======================================================================== */
/*  SYS.C — システム制御およびブザー制御                                      */
/* ======================================================================== */

#include "sys.h"
#include "memmap.h"
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

/* ホットデプロイ窓の直下にさらに固定予約した量 (バイト、ページ境界)。
 * 現状の唯一の利用者は PEGC 8bpp バックバッファ (H2)。9801 では 0 のまま
 * なので sys_usable_mem_end() は従来と同じ値を返す (回帰ゼロ)。 */
static u32 sys_top_reserved = 0;

/* ステージング領域の先頭 (物理)。物理末尾からホットデプロイ窓を引いた位置で、
 * 追加予約 (sys_top_reserved) には影響されない。
 * 設計: docs/tasks/hotdeploy/DESIGN.md */
u32 sys_hotdeploy_base(void)
{
    u32 end = sys_mem_kb * 1024;
    return (end > MEM_HOTDEPLOY_SIZE * 2) ? (end - MEM_HOTDEPLOY_SIZE) : end;
}

/* 物理末尾からホットデプロイ用ステージング領域と固定予約を除いた、
 * 割り当ててよい上限。子プロセスのスタックはここから下へ伸びる。 */
u32 sys_usable_mem_end(void)
{
    return sys_hotdeploy_base() - sys_top_reserved;
}

/* ======================================================================== */
/*  sys_reserve_top — 物理末尾側に固定領域を切り出す (GUI v1.1 H2)          */
/*                                                                          */
/*  ホットデプロイ窓の直下から bytes バイト (4KB 切り上げ) を予約し、その    */
/*  先頭物理アドレスを返す。以後 sys_usable_mem_end() はその分だけ下がるので */
/*  exec の子プロセス (コード/ヒープ/スタック) はここへ伸びてこない。        */
/*                                                                          */
/*  **exec_run より前 (ブート中) に 1 回だけ呼ぶこと。** 子プロセスが走って  */
/*  いる最中に上限を動かすと、その子のレイアウトと pgalloc の予約範囲が      */
/*  食い違う。同じサイズでの再呼び出しは冪等 (同じ先頭を返す)。             */
/*                                                                          */
/*  戻り値: 予約領域の先頭物理アドレス。0 = 予約できなかった                 */
/*  (メモリ不足、または既に別サイズで予約済み)。                             */
/* ======================================================================== */
u32 sys_reserve_top(u32 bytes)
{
    u32 top;
    u32 need = (bytes + 0xFFFUL) & ~0xFFFUL;

    if (need == 0) return 0;

    /* 冪等: 同じ量なら既存の予約をそのまま返す */
    if (sys_top_reserved != 0) {
        return (sys_top_reserved == need) ? (sys_hotdeploy_base() - need) : 0;
    }

    top = sys_hotdeploy_base();

    /* 予約後もプログラム帯 (0x500000〜) にスタック + 最低限のヒープが
     * 残ることを確かめる。ここを割ると exec が子を載せられなくなる。 */
    if (top < MEM_EXEC_LOAD_ADDR + need +
              MEM_EXEC_STACK_SIZE + MEM_EXEC_SBRK_MIN + MEM_EXEC_HEAP_MIN) {
        return 0;
    }

    sys_top_reserved = need;
    return top - need;
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
