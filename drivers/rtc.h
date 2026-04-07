/* ======================================================================== */
/*  RTC.H — µPD4990A カレンダ時計ドライバ                                   */
/*                                                                          */
/*  PC-9801内蔵RTC (µPD4990A) をI/Oポート直接制御で操作                      */
/*  シリアルビットバンギングで48ビットBCD時刻データを読み書き                 */
/*                                                                          */
/*  出典: PC9800Bible §2-4                                                  */
/*  参照: FreeBSD sys/pc98/cbus/pcrtc.c                                     */
/* ======================================================================== */

#ifndef __RTC_H
#define __RTC_H

#include "types.h"

/* ======== I/Oポート定義 ======== */
#define RTC_SET    0x20    /* コマンド/データ書込みポート */
#define RTC_READ   0x33    /* データ読み出しポート (bit0 = DO) */

/* ======== ポート0x20 ビット配置 ======== */
/*   bit5: DI  (Data Input)   */
/*   bit4: CLK (Clock)        */
/*   bit3: STB (Strobe)       */
/*   bit2-0: C2-C0 (Command)  */
/* FreeBSD互換定義 (PC98.h)   */
#define RTC_DI     0x20    /* bit5: データ入力 */
#define RTC_CLK    0x10    /* bit4: クロック */
#define RTC_STB    0x08    /* bit3: ストローブ */

/* ======== シリアルコマンド (C3-C0) ======== */
/* C0→C1→C2→C3の順にシフトイン */
#define RTC_CMD_HOLD       0x00   /* 0000: レジスタホールド */
#define RTC_CMD_SHIFT      0x01   /* 0001: レジスタシフト */
#define RTC_CMD_TIMESET    0x02   /* 0010: タイムセット */
#define RTC_CMD_TIMEREAD   0x03   /* 0011: タイムリード */

#define RTC_READ_BITS      48     /* RTCから読み出すビット数 */

/* ======== 時刻構造体 ======== */
typedef struct {
    u8 year;     /* 00-99 (BCD → 変換済み10進) */
    u8 month;    /* 1-12 */
    u8 day;      /* 1-31 */
    u8 wday;     /* 0=日, 1=月, ..., 6=土 */
    u8 hour;     /* 0-23 */
    u8 min;      /* 0-59 */
    u8 sec;      /* 0-59 */
} RTC_Time;

/* ======== API ======== */

/* RTC初期化 (タイムリードモードに設定) */
void rtc_init(void);

/* 現在日時を読み出す */
void rtc_read(RTC_Time *t);

/* uptimeをtick_countから計算し秒で返す */
u32  rtc_uptime_sec(void);

#endif /* __RTC_H */
