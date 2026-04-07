/* ======================================================================== */
/*  RTC.C — µPD4990A カレンダ時計ドライバ                                   */
/*                                                                          */
/*  シリアルビットバンギングで48ビットBCD時刻データを読み出す                 */
/*  データ形式 (LSBファースト, 各4ビット):                                   */
/*    1秒, 10秒, 1分, 10分, 1時, 10時, 1日, 10日, 曜日, 月, 1年, 10年       */
/*                                                                          */
/*  出典: PC9800Bible §2-4                                                  */
/*  参照: FreeBSD sys/pc98/cbus/pcrtc.c                                     */
/* ======================================================================== */

#include "rtc.h"
#include "io.h"
#include "memmap.h"

/* 外部: tick_count (idt.c) */
extern volatile u32 tick_count;

/* ======== I/Oウェイト ======== */
/* io_wait()×2回で約1.2µsの遅延 */
static void rtc_wait(void)
{
    io_wait();
    io_wait();
}

/* ======== コマンド送信 ======== */
/* µPD4990AにシリアルコマンドC0-C3を送信 */
/* C0→C1→C2→C3の順にDIラインからシフトイン */
static void rtc_send_cmd(u8 cmd)
{
    int i;

    /* STB=0で開始 */
    outp(RTC_SET, 0x00);
    rtc_wait();

    /* コマンドビット C0→C3 の順にシフトイン */
    for (i = 0; i < 4; i++) {
        u8 di = (cmd & (1 << i)) ? RTC_DI : 0;

        /* CLK=0, DI設定 */
        outp(RTC_SET, di);
        rtc_wait();

        /* CLK=1 (立ち上がりでデータラッチ) */
        outp(RTC_SET, di | RTC_CLK);
        rtc_wait();
    }

    /* CLK=0に戻す */
    outp(RTC_SET, 0x00);
    rtc_wait();

    /* STBパルス (コマンドラッチ) */
    outp(RTC_SET, RTC_STB);
    rtc_wait();
    outp(RTC_SET, 0x00);
    rtc_wait();
}

/* ======== データ読み出し (48ビット) ======== */
/* µPD4990Aのシフトレジスタから48ビットを読み出す */
/* LSBファーストで各4ビット×12ニブル */
static void rtc_read_48bits(u8 *nibbles)
{
    int i;

    for (i = 0; i < RTC_READ_BITS; i++) {
        u8 bit;

        /* CLK=1 */
        outp(RTC_SET, RTC_CLK);
        rtc_wait();

        /* データ読み出し (bit0 = DO) */
        bit = (u8)(inp(RTC_READ) & 0x01);

        /* CLK=0 */
        outp(RTC_SET, 0x00);
        rtc_wait();

        /* ニブル配列に格納 */
        if ((i & 3) == 0) {
            nibbles[i >> 2] = bit;
        } else {
            nibbles[i >> 2] |= (bit << (i & 3));
        }
    }
}

/* ======================================================================== */
/*  公開API                                                                 */
/* ======================================================================== */

void rtc_init(void)
{
    /* タイムリードモードに設定 */
    rtc_send_cmd(RTC_CMD_TIMEREAD);
}

void rtc_read(RTC_Time *t)
{
    u8 nib[12];   /* 12ニブル (48ビット) */

    /* レジスタホールド (読み出し中に値が変わらないようにする) */
    rtc_send_cmd(RTC_CMD_HOLD);

    /* タイムリード */
    rtc_send_cmd(RTC_CMD_TIMEREAD);

    /* 48ビット読み出し */
    rtc_read_48bits(nib);

    /* レジスタシフト (通常カウント再開) */
    rtc_send_cmd(RTC_CMD_SHIFT);

    /*
     * ニブル配置 (LSBファースト):
     *  nib[0]  = 1秒    nib[1]  = 10秒
     *  nib[2]  = 1分    nib[3]  = 10分
     *  nib[4]  = 1時    nib[5]  = 10時
     *  nib[6]  = 1日    nib[7]  = 10日
     *  nib[8]  = 曜日   nib[9]  = 月
     *  nib[10] = 1年    nib[11] = 10年
     */
    t->sec   = (nib[1] & 0x07) * 10 + (nib[0] & 0x0F);
    t->min   = (nib[3] & 0x07) * 10 + (nib[2] & 0x0F);
    t->hour  = (nib[5] & 0x03) * 10 + (nib[4] & 0x0F);
    t->day   = (nib[7] & 0x03) * 10 + (nib[6] & 0x0F);
    t->wday  = nib[8] & 0x07;
    t->month = nib[9] & 0x0F;
    t->year  = (nib[11] & 0x0F) * 10 + (nib[10] & 0x0F);

    /* 範囲チェック */
    if (t->sec > 59) t->sec = 0;
    if (t->min > 59) t->min = 0;
    if (t->hour > 23) t->hour = 0;
    if (t->day < 1 || t->day > 31) t->day = 1;
    if (t->month < 1 || t->month > 12) t->month = 1;
}

u32 rtc_uptime_sec(void)
{
    return tick_count / PIT_HZ;  /* PIT_HZ Hz → 秒変換 */
}
