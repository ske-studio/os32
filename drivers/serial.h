/* ======================================================================== */
/*  SERIAL.H — PC-98 RS-232C シリアル通信ドライバ定義                       */
/*                                                                          */
/*  μPD8251A (USART) 内蔵RS-232Cポート制御                                 */
/*  出典: PC9800Bible §2-10, FreeBSD sys/pc98/cbus/sio.c                   */
/* ======================================================================== */

#ifndef __SERIAL_H
#define __SERIAL_H

#include "types.h"

/* ======== I/Oポート (FreeBSD if_8251_type[COM_IF_INTERNAL]) ======== */
#define SER_DATA    0x30    /* [0] 送受信データ */
#define SER_CMD     0x32    /* [1] コマンドライト / [2] ステータスリード */
#define SER_SIGNAL  0x33    /* [3] モデム信号線 (CI/CS/CD) */
#define SER_MASK    0x35    /* [4] 割り込みマスク */

/* PIT カウンタ#2 (ボーレート設定) */
#define SER_TIMER_CNT   0x75    /* カウンタ#2 データ */
#define SER_TIMER_MODE  0x77    /* PIT モードレジスタ */
#define SER_IO_WAIT     0x5F    /* I/Oウェイト (FreeBSD準拠) */

/* ======== ステータスビット (ポート0x32 リード) ======== */
#define STS_TXRDY   0x01    /* D0: 第2送信バッファ空 (送信可) */
#define STS_RXRDY   0x02    /* D1: 受信データあり */
#define STS_TXE     0x04    /* D2: 送信バッファ全空 */
#define STS_PE      0x08    /* D3: パリティエラー */
#define STS_OE      0x10    /* D4: オーバーランエラー */
#define STS_FE      0x20    /* D5: フレーミングエラー */
#define STS_BRK     0x40    /* D6: ブレーク検出 */
#define STS_DSR     0x80    /* D7: DSR信号 */

/* ======== コマンドビット (ポート0x32 ライト) ======== */
#define CMD_TXE     0x01    /* D0: 送信イネーブル */
#define CMD_DTR     0x02    /* D1: DTR (負論理) */
#define CMD_RXE     0x04    /* D2: 受信イネーブル */
#define CMD_SBRK    0x08    /* D3: ブレーク送信 */
#define CMD_ER      0x10    /* D4: エラーリセット */
#define CMD_RTS     0x20    /* D5: RTS (負論理) */
#define CMD_RESET   0x40    /* D6: 内部リセット */

/* ======== モードビット (ポート0x32, リセット直後の1回目) ======== */
/* D1-D0: 分周比 */
#define MOD_CLKx1   0x01    /* ×1モード */
#define MOD_CLKx16  0x02    /* ×16モード */
#define MOD_CLKx64  0x03    /* ×64モード */
/* D3-D2: キャラクタ長 */
#define MOD_5BIT    0x00
#define MOD_6BIT    0x04
#define MOD_7BIT    0x08
#define MOD_8BIT    0x0C
/* D4: パリティイネーブル */
#define MOD_PENAB   0x10
/* D5: パリティ種別 */
#define MOD_PEVEN   0x20
/* D7-D6: ストップビット */
#define MOD_STOP1   0x40
#define MOD_STOP15  0x80
#define MOD_STOP2   0xC0

/* ======== 割り込みマスクビット (ポート0x35) ======== */
#define IEN_RX      0x01    /* D0: 受信レディ割り込み */
#define IEN_TXEMP   0x02    /* D1: 送信エンプティ割り込み */
#define IEN_TX      0x04    /* D2: 送信レディ割り込み */

/* ======== モデム信号ビット (ポート0x33 リード) ======== */
#define SIG_CD      0x20    /* D5: CD (負論理) */
#define SIG_CS      0x40    /* D6: CS/CTS (負論理) */
#define SIG_CI      0x80    /* D7: CI/RI (負論理) */

/* ======== システムクロック定数 — UNDOCUMENTED io_tcu.md 準拠 ======== */
/* PC9800BibleとUNDOCUMENTEDでMHz系とクロック値の対応が逆転している。       */
/* ここでは両方の値を定義し、実行時にポート42h bit5等で判別すべき。          */
/* 現在のOS32はNP21/W上で1996800Hzとして動作確認済み。                       */
#define TIMER_CLK_1997  1996800UL   /* 1.9968MHz (NP21/Wデフォルト) */
#define TIMER_CLK_2458  2457600UL   /* 2.4576MHz */

/* ======== 受信バッファ ======== */
#define SER_BUF_SIZE    4096

/* ======== 公開API ======== */
void serial_init(unsigned long baud);
void serial_putchar(char c);
void serial_puts(const char *str);
int  serial_getchar(void);     /* ブロッキング */
int  serial_trygetchar(void);  /* ノンブロッキング: -1=なし */
int  serial_has_data(void);    /* 受信バッファにデータがあるか */
int  serial_is_initialized(void);

/* IRQ4ハンドラ (ASMスタブから呼ばれる) */
void serial_irq_handler(void);

#endif /* __SERIAL_H */
