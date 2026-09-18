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

/* ======================================================================== */
/*  システムクロック — 8251 の速度は 8253 TCU カウンタ#2 の分周で決まる      */
/*                                                                          */
/*  判定は BIOS ワークエリア **0000:0501h bit 7**:                          */
/*    1 = 8MHz系    → タイマクロック 1.9968MHz                              */
/*    0 = 5/10MHz系 → タイマクロック 2.4576MHz                              */
/*                                                                          */
/*  **資料が食い違うので根拠を残す** (2026-09-18 に決着):                    */
/*  `docs/hw/undocumented/io_tcu.md` の表は上下 2 行が逆になっているが、     */
/*  `memsys.md` の 0000:0501h の項は自己整合する — 併記された SCLK1 の表が   */
/*  「値1 → 7.9872MHz / 値0 → 9.8304MHz」で、4 分周すると 1.9968 / 2.4576 に */
/*  なる。よって **memsys.md を採る** (io_tcu.md の 2 行は転記ミス)。        */
/*  FreeBSD の pc98_ttspeedtab (8MHz系 = 1996800) も memsys.md と一致する。  */
/*                                                                          */
/*  **ちょうど出る速度はクロックで変わる** (clk/16 が baud で割り切れるか):   */
/*    1.9968MHz (clk/16 = 124800): 9600 のみ。19200 も 38400 も割り切れない  */
/*                                 (38400 → count 3.25 → 実効 41600、+8.3%) */
/*    2.4576MHz (clk/16 = 153600): 9600 / 19200 / 38400 すべてちょうど       */
/* ======================================================================== */
#define BIOS_WORK_SYSCLK    0x00000501UL  /* BYTE: bit7=1 なら 8MHz系 */
#define BIOS_SYSCLK_8MHZ    0x80

#define TIMER_CLK_1997  1996800UL   /* 8MHz系 (0501h bit7 = 1) */
#define TIMER_CLK_2458  2457600UL   /* 5/10MHz系 (0501h bit7 = 0) */

/* ======================================================================== */
/*  拡張RS-232C制御レジスタ (I/O 0434h、Undocumented io_rs.md)              */
/*                                                                          */
/*  PC-9801P･NX/C、PC-9821Af･Ne 以降は 8251 のクロックが 2.4576 →           */
/*  9.8304MHz に変わり、19200bps 対応機には**入力クロック 4 分周回路**が     */
/*  ここに入っている。外せば 4 倍の速度が出せる。                            */
/*                                                                          */
/*  ⚠ **極性が機種依存で、しかも資料に異論が併記されている。**               */
/*    群 A (大半の PC-9821): 1 = 4分周しない / 0 = する (既定)              */
/*    群 B (An･Ap3･As3･Xa･Xt･Xf･Cf･Xa10･Xa9･Xa7･Xt13･Xa12･Xa7e･Na7･Nx):    */
/*          1 = しない (既定) / 0 = する                                    */
/*          — ただし 1999 年の読者指摘で「1 = する (既定) / 0 = しない」が   */
/*            正しいのではないか、と併記されている。**未決着。**             */
/*                                                                          */
/*  ⚠ **bit0 は「プライマリシリアルポート切り離し」。** 雑に書くとポートが   */
/*    消える。必ず read-modify-write で bit0 を保つこと。                    */
/*                                                                          */
/*  だから **自動では触らない。** 実機でどちらの向きか確かめたうえで         */
/*  `serial_set_div4()` を明示的に呼ぶ。                                     */
/* ======================================================================== */
#define SER_EXT_CTRL        0x0434
#define SER_EXT_DIV4        0x40    /* bit6: 入力クロック 4 分周 (極性注意) */
#define SER_EXT_DISCONNECT  0x01    /* bit0: ポート切り離し — 保つ */

/* 設定の結果。**要求どおりに出ないことがあるので呼び手に見せる** ([V4])。 */
struct serial_setup {
    unsigned long want;     /* 要求した速度 */
    unsigned long actual;   /* 実際に出る速度 (clk / 16 / count) */
    unsigned long clk;      /* 使ったタイマクロック */
    u16 count;              /* 8253 カウンタ#2 の分周比 */
    u8  sysclk_8mhz;        /* 0000:0501h bit7 の値 */
    u8  exact;              /* 1 = ちょうど出る / 0 = ずれている */
};

/* ======== 受信バッファ ======== */
#define SER_BUF_SIZE    4096

/* ======== 公開API ======== */

/* システムクロックを BIOS ワークエリアから判定してキャッシュする。
 *
 * **カーネル初期化から 1 回だけ呼ぶ。** `serial_init` は KAPI 経由 (CPL=3 の
 * アプリ文脈、CR3 はアプリの PD) でしか呼ばれないので、そこから物理 0x501 を
 * 読むのは安全でない。呼ばれていなければ `serial_init` は 1.9968MHz を使う
 * (従来の挙動)。 */
void serial_detect_clock(void);

/* 直前の `serial_init` の結果。まだ呼ばれていなければ want=0。 */
const struct serial_setup *serial_get_setup(void);

/* I/O 0434h の生読み。-1 = 読めない / 0..255 = 値。
 * **FFh かどうかで搭載を判断しないこと** (00BEh のデコードイメージが出る
 * 機種がある。io_fdd.md / io_rs.md の注意)。 */
int serial_get_ext_ctrl(void);

/* 0434h bit6 を書く (read-modify-write で bit0 を保つ)。
 * 戻りは読み戻した 0434h の値、または -1。
 * **極性は機種依存で未決着** (上の注記)。自動では呼ばれない。 */
int serial_set_div4(int bit_value);

void serial_init(unsigned long baud);
void serial_putchar(char c);
void serial_puts(const char *str);
void serial_puts_polled(const char *str);
void serial_put_hex32_polled(u32 val);
int  serial_getchar(void);     /* ブロッキング */
int  serial_trygetchar(void);  /* ノンブロッキング: -1=なし */
int  serial_peekchar(void);    /* 覗くだけ (取り出さない): -1=なし */
int  serial_has_data(void);    /* 受信バッファにデータがあるか */
int  serial_is_initialized(void);

/* IRQ4ハンドラ (ASMスタブから呼ばれる) */
void serial_irq_handler(void);

#endif /* __SERIAL_H */
